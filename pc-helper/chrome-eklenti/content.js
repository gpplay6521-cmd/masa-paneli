// YouTube Music sayfasında çalışır. background.js'ten gelen komuta göre gerçek düğmeye tıklar.
// Seçiciler yerine erişilebilirlik adına (aria-label/title) göre arama yapılır: sayfa güncellenip
// iç yapı (class/id) değişse bile "Karıştır"/"Tekrarla" yazısı kaldıkça çalışmaya devam eder.
// Sayfa gölge DOM (shadow DOM, Polymer bileşenleri) kullandığı için arama gölge köklerin içine de iner.

function deepQueryAll(root, pred, out = []) {
  const walk = (node) => {
    if (!node) return;
    if (node.nodeType === 1 && pred(node)) out.push(node);
    const children = node.shadowRoot ? node.shadowRoot.children : node.children;
    if (children) for (const c of children) walk(c);
  };
  walk(root);
  return out;
}

function accessibleText(el) {
  return ((el.getAttribute("aria-label") || el.getAttribute("title") || "") + "").toLowerCase();
}

function clickByLabel(patterns) {
  const matches = deepQueryAll(document.body, (el) => {
    if (el.tagName !== "BUTTON" && el.getAttribute("role") !== "button") return false;
    const t = accessibleText(el);
    return t && patterns.some((p) => t.includes(p));
  });
  if (matches.length) { matches[0].click(); return true; }
  return false;
}

function like() {
  // Beğen/Beğenme çifti bu sarmalayıcının içinde; ilk düğme başparmak-yukarı (beğen).
  const wrapper = document.querySelector("ytmusic-like-button-renderer#like-button-renderer");
  if (!wrapper) return clickByLabel(["beğen", "like this"]);
  const btns = deepQueryAll(wrapper, (el) => el.tagName === "BUTTON");
  if (btns.length) { btns[0].click(); return true; }
  return false;
}

chrome.runtime.onMessage.addListener((msg) => {
  if (!msg || !msg.cmd) return;
  if (msg.cmd === "like") like();
  else if (msg.cmd === "shuffle") clickByLabel(["karıştır", "shuffle"]);
  else if (msg.cmd === "repeat") clickByLabel(["tekrar", "repeat"]);
});
