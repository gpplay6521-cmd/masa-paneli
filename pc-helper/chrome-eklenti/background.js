// Bilgisayardaki masa paneli yardımcı programına (panel_helper.py) yerel WebSocket ile bağlanır.
// Panelde Beğen/Karıştır/Tekrarla'ya dokununca yardımcı program buraya bir komut yollar, biz de
// açık olan YouTube Music sekmesine iletiriz (content.js sayfada tıklamayı yapar).
const WS_URL = "ws://127.0.0.1:8934";
let socket = null;

function connect() {
  if (socket && (socket.readyState === WebSocket.OPEN || socket.readyState === WebSocket.CONNECTING)) return;
  try {
    socket = new WebSocket(WS_URL);
  } catch (e) {
    socket = null;
    return;
  }
  socket.onmessage = (event) => {
    let msg;
    try { msg = JSON.parse(event.data); } catch (e) { return; }
    if (!msg || !msg.cmd) return;
    chrome.tabs.query({ url: "https://music.youtube.com/*" }, (tabs) => {
      for (const tab of tabs) {
        chrome.tabs.sendMessage(tab.id, msg, () => void chrome.runtime.lastError);
      }
    });
  };
  socket.onclose = () => { socket = null; };
  socket.onerror = () => { try { socket.close(); } catch (e) { /* yoksay */ } };
}

// Manifest V3'te arka plan servis çalışanı boşta kalınca kapanabilir; alarm onu periyodik
// uyandırıp bağlantının düşmüş olup olmadığını kontrol ettirir (düştüyse yeniden bağlanır).
chrome.alarms.create("baglanti-kontrol", { periodInMinutes: 0.4 });
chrome.alarms.onAlarm.addListener((a) => { if (a.name === "baglanti-kontrol") connect(); });
chrome.runtime.onInstalled.addListener(connect);
chrome.runtime.onStartup.addListener(connect);
connect();
