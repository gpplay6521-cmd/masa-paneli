"""Sistem izleme: panelin takılı olduğu bilgisayarın işlemci, bellek ve ekran kartı bilgileri.

Kaynaklar (yönetici izni gerektirmez):
  işlemci/bellek : psutil
  ekran kartı    : NVIDIA sürücüsündeki NVML (nvidia-ml-py; kullanım, sıcaklık, VRAM, güç);
                   NVML yoksa (AMD/Intel vb.) Windows performans sayaçları "GPU Engine" ve "GPU Adapter Memory"
                   (PDH; kullanım ve VRAM kullanımı) ve kayıt defterindeki bağdaştırıcı bilgisi (ad, VRAM toplamı).
  ağ             : psutil (tüm ağ bağdaştırıcılarının toplam indirme/yükleme hızı); gecikme = internete TCP bağlantı süresi
                   (varsayılan 1.1.1.1:443; panel_ayarlari.json "ping_hedef")
  işlemci sıcaklığı: Windows bunu yönetici izni/üçüncü parti sürücü olmadan vermez (MSAcpi_ThermalZoneTemperature
                   bu bilgisayarda "Access denied" döndü) -> bilinmiyor (-999) gönderilir, panel gizler.
Bilinmeyen değerler NA (-999) olarak taşınır.
"""
import ctypes
import re
import socket
import time
import winreg
from ctypes import wintypes as wt

import psutil

NA = -999


def _clean_cpu(name):
    n = re.sub(r"\((R|TM|r|tm)\)", "", name or "")
    n = re.sub(r"\s*@\s*[\d.]+\s*GHz", "", n, flags=re.I)
    n = re.sub(r"\b(CPU|Processor)\b", "", n, flags=re.I)
    n = re.sub(r"\b\d+-Core\b", "", n, flags=re.I)
    n = re.sub(r"\s+with\s+.*$", "", n, flags=re.I)
    return re.sub(r"\s+", " ", n).strip()


def _clean_gpu(name):
    return re.sub(r"\s+", " ", re.sub(r"^NVIDIA\s+", "", name or "", flags=re.I)).strip()


def _cpu_name():
    try:
        with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, r"HARDWARE\DESCRIPTION\System\CentralProcessor\0") as k:
            return _clean_cpu(winreg.QueryValueEx(k, "ProcessorNameString")[0])
    except OSError:
        return ""


def _registry_adapters():
    """Ekran bağdaştırıcıları: [(ad, VRAM bayt)]"""
    out = []
    base = r"SYSTEM\CurrentControlSet\Control\Class\{4d36e968-e325-11ce-bfc1-08002be10318}"
    try:
        with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, base) as root:
            i = 0
            while True:
                try:
                    sub = winreg.EnumKey(root, i)
                except OSError:
                    break
                i += 1
                if not sub.isdigit():
                    continue
                try:
                    with winreg.OpenKey(root, sub) as k:
                        desc = winreg.QueryValueEx(k, "DriverDesc")[0]
                        try:
                            mem = int(winreg.QueryValueEx(k, "HardwareInformation.qwMemorySize")[0])
                        except (OSError, ValueError, TypeError):
                            mem = 0
                        out.append((desc, mem))
                except OSError:
                    continue
    except OSError:
        pass
    return out


class NvmlGpu:
    name_source = "NVML"

    def __init__(self):
        import pynvml
        self.nv = pynvml
        pynvml.nvmlInit()
        best, best_mem = None, -1
        for i in range(pynvml.nvmlDeviceGetCount()):       # birden çok kart varsa en çok belleklisi
            h = pynvml.nvmlDeviceGetHandleByIndex(i)
            mem = pynvml.nvmlDeviceGetMemoryInfo(h).total
            if mem > best_mem:
                best, best_mem = h, mem
        if best is None:
            raise RuntimeError("NVML: ekran kartı yok")
        self.h = best
        name = pynvml.nvmlDeviceGetName(best)
        self.name = _clean_gpu(name.decode() if isinstance(name, bytes) else name)

    def sample(self):
        nv, h = self.nv, self.h
        r = {"gpu": NA, "gpu_temp": NA, "vram": NA, "vram_used": NA, "vram_total": NA, "gpu_power": NA}
        try:
            r["gpu"] = int(nv.nvmlDeviceGetUtilizationRates(h).gpu)
        except Exception:  # noqa: BLE001
            pass
        try:
            r["gpu_temp"] = int(nv.nvmlDeviceGetTemperature(h, nv.NVML_TEMPERATURE_GPU))
        except Exception:  # noqa: BLE001
            pass
        try:
            m = nv.nvmlDeviceGetMemoryInfo(h)
            r["vram_used"], r["vram_total"] = int(m.used // 2**20), int(m.total // 2**20)
            r["vram"] = int(round(m.used * 100 / m.total)) if m.total else NA
        except Exception:  # noqa: BLE001
            pass
        try:
            r["gpu_power"] = int(round(nv.nvmlDeviceGetPowerUsage(h) / 1000))
        except Exception:  # noqa: BLE001
            pass
        return r


# ---- PDH (Windows performans sayaçları), ctypes ile ----
_PDH_FMT_DOUBLE = 0x200
_PDH_MORE_DATA = 0x800007D2


class _Val(ctypes.Union):
    _fields_ = [("longValue", ctypes.c_long), ("doubleValue", ctypes.c_double), ("largeValue", ctypes.c_longlong)]


class _FmtVal(ctypes.Structure):
    _fields_ = [("CStatus", wt.DWORD), ("v", _Val)]


class _Item(ctypes.Structure):
    _fields_ = [("szName", wt.LPWSTR), ("FmtValue", _FmtVal)]


class PdhGpu:
    name_source = "PDH"

    def __init__(self):
        self.pdh = ctypes.WinDLL("pdh")
        self.q = wt.HANDLE()
        if self.pdh.PdhOpenQueryW(None, 0, ctypes.byref(self.q)) != 0:
            raise RuntimeError("PDH sorgusu açılamadı")
        self.c_eng = self._add(r"\GPU Engine(*)\Utilization Percentage")
        self.c_mem = self._add(r"\GPU Adapter Memory(*)\Dedicated Usage")
        self.pdh.PdhCollectQueryData(self.q)
        ads = [a for a in _registry_adapters() if a[0]]
        ads.sort(key=lambda a: a[1], reverse=True)
        self.name = _clean_gpu(ads[0][0]) if ads else ""
        self.total_mb = int(ads[0][1] // 2**20) if ads and ads[0][1] else NA

    def _add(self, path):
        c = wt.HANDLE()
        if self.pdh.PdhAddEnglishCounterW(self.q, path, 0, ctypes.byref(c)) != 0:
            raise RuntimeError(f"PDH sayacı yok: {path}")
        return c

    def _array(self, counter):
        size, count = wt.DWORD(0), wt.DWORD(0)
        r = self.pdh.PdhGetFormattedCounterArrayW(counter, _PDH_FMT_DOUBLE, ctypes.byref(size), ctypes.byref(count), None)
        if (r & 0xFFFFFFFF) != _PDH_MORE_DATA:
            return []
        buf = ctypes.create_string_buffer(size.value)
        if self.pdh.PdhGetFormattedCounterArrayW(counter, _PDH_FMT_DOUBLE, ctypes.byref(size), ctypes.byref(count), buf) != 0:
            return []
        items = ctypes.cast(buf, ctypes.POINTER(_Item))
        return [(items[i].szName, items[i].FmtValue.v.doubleValue) for i in range(count.value)]

    def sample(self):
        r = {"gpu": NA, "gpu_temp": NA, "vram": NA, "vram_used": NA, "vram_total": self.total_mb, "gpu_power": NA}
        if self.pdh.PdhCollectQueryData(self.q) != 0:
            return r
        # Motor başına kullanım = o motoru kullanan tüm işlemlerin toplamı; kart kullanımı = en yüklü motor (Görev Yöneticisi gibi)
        engines = {}
        for name, val in self._array(self.c_eng):
            m = re.search(r"luid_(0x[0-9a-fA-F]+_0x[0-9a-fA-F]+)_phys_(\d+)_eng_(\d+)", name or "")
            if m:
                engines[(m.group(1), m.group(2), m.group(3))] = engines.get((m.group(1), m.group(2), m.group(3)), 0.0) + val
        mem = {}
        for name, val in self._array(self.c_mem):
            m = re.search(r"luid_(0x[0-9a-fA-F]+_0x[0-9a-fA-F]+)", name or "")
            if m:
                mem[m.group(1)] = max(mem.get(m.group(1), 0.0), val)
        luid = max(mem, key=mem.get) if mem else None     # en çok ayrılmış belleği kullanan kart
        if luid is None and engines:
            luid = max({k[0] for k in engines}, key=lambda l: sum(v for k, v in engines.items() if k[0] == l))
        if luid is not None:
            loads = [v for k, v in engines.items() if k[0] == luid]
            if loads:
                r["gpu"] = int(round(min(100.0, max(loads))))
            if luid in mem:
                used = int(mem[luid] // 2**20)
                r["vram_used"] = used
                if self.total_mb != NA and self.total_mb > 0:
                    r["vram"] = int(round(min(100.0, used * 100 / self.total_mb)))
        return r


class SysMon:
    def __init__(self, backend="auto"):
        self.host = socket.gethostname()[:38]
        self.cpu_name = _cpu_name()
        self.threads = psutil.cpu_count(logical=True) or 0
        self.gpu = None
        self.gpu_error = ""
        order = {"auto": ("nvml", "pdh"), "nvml": ("nvml",), "pdh": ("pdh",), "none": ()}[backend]
        for kind in order:
            try:
                self.gpu = NvmlGpu() if kind == "nvml" else PdhGpu()
                break
            except Exception as e:  # noqa: BLE001
                self.gpu_error += f"{kind}: {e}; "
        psutil.cpu_percent(None)    # ilk çağrı yalnızca referans alır
        self._net = psutil.net_io_counters()
        self._net_t = time.time()
        self.ping_ms = NA

    @property
    def gpu_name(self):
        return self.gpu.name if self.gpu else ""

    def describe(self):
        gpu = f"{self.gpu_name} ({self.gpu.name_source})" if self.gpu else f"yok ({self.gpu_error.strip()})"
        return f"bilgisayar={self.host}, işlemci={self.cpu_name} ({self.threads} iş parçacığı), ekran kartı={gpu}"

    def net_rates(self):
        """(indirme, yükleme) kbps: bir önceki çağrıdan bu yana."""
        now, c = time.time(), psutil.net_io_counters()
        dt = max(now - self._net_t, 0.001)
        down = int((c.bytes_recv - self._net.bytes_recv) * 8 / 1000 / dt)
        up = int((c.bytes_sent - self._net.bytes_sent) * 8 / 1000 / dt)
        self._net, self._net_t = c, now
        return max(down, 0), max(up, 0)

    def measure_ping(self, host, port=443, timeout=1.0):
        """İnternete TCP bağlantı süresi (ms) = yaklaşık gidiş-dönüş gecikmesi; ulaşılamazsa NA."""
        t0 = time.perf_counter()
        try:
            with socket.create_connection((host, port), timeout=timeout):
                pass
            self.ping_ms = max(1, int(round((time.perf_counter() - t0) * 1000)))
        except OSError:
            self.ping_ms = NA
        return self.ping_ms

    def sample(self):
        """@M satırının değerleri (sıra protokolle aynı)."""
        vm = psutil.virtual_memory()
        g = self.gpu.sample() if self.gpu else {"gpu": NA, "gpu_temp": NA, "vram": NA, "vram_used": NA,
                                                "vram_total": NA, "gpu_power": NA}
        down, up = self.net_rates()
        return [int(round(psutil.cpu_percent(None))), NA, int(round(vm.percent)),
                int((vm.total - vm.available) // 2**20), int(vm.total // 2**20),
                g["gpu"], g["gpu_temp"], g["vram"], g["vram_used"], g["vram_total"], g["gpu_power"],
                down, up, self.ping_ms]
