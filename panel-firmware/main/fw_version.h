#pragma once

/* Panel yazılımının sürümü (YYYY.AA.GG.n). Karta yüklenecek her yeni derlemede ARTIRILIR.
 * Panel bunu bilgisayara bildirir (açılışta "@P", "@?" yanıtında "@!"); bilgisayardaki yardımcı program kendi paketindeki (pc-helper\firmware\)
 * yazılımın sürümü panelinkinden yeniyse yeni yazılımı panele kendiliğinden yükler (pc-helper\panel_yukle.py).
 * Sürüm karşılaştırması noktalarla ayrılmış sayılarla yapılır. pc-helper\firmware_hazirla.py bu dosyadaki sürümün derlenmiş
 * yazılımın içinde de geçtiğini denetler (eski derlemeyi yanlışlıkla paketlemeyi engeller). */
#define FW_VERSION "2026.09.20.12"
