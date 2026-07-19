/* Multi-language code tabs for the Cloud-Native Design Patterns book.
 *
 * No framework, no plugin, GitHub-Pages-safe. For each <div class="codetabs">
 * marker emitted by _includes/codetabs.html, absorb the following N Rouge
 * code blocks (N = number of pipe-separated labels) into a tab group.
 * Selecting a language syncs every group on the page and is remembered in
 * localStorage so a reader who picks ".NET" keeps seeing .NET everywhere.
 */
(function () {
  "use strict";
  var KEY = "lsp-lang";
  var groups = [];

  function isCodeBlock(el) {
    if (!el || el.nodeType !== 1) return false;
    if (el.classList && el.classList.contains("highlighter-rouge")) return true;
    return el.tagName === "DIV" && el.querySelector && !!el.querySelector(".highlight");
  }

  function build(marker) {
    var langs = (marker.getAttribute("data-langs") || "")
      .split("|").map(function (s) { return s.trim(); }).filter(Boolean);
    if (!langs.length) { marker.parentNode.removeChild(marker); return null; }

    var blocks = [], el = marker.nextElementSibling;
    while (el && blocks.length < langs.length) {
      if (isCodeBlock(el)) blocks.push(el);
      else if (el.tagName !== "P" || el.textContent.trim() !== "") break;
      el = el.nextElementSibling;
    }
    if (!blocks.length) { marker.parentNode.removeChild(marker); return null; }
    langs = langs.slice(0, blocks.length);

    var wrap = document.createElement("div");
    wrap.className = "codetabs-wrap";
    var bar = document.createElement("div");
    bar.className = "codetabs-bar";
    bar.setAttribute("role", "tablist");
    var panels = document.createElement("div");
    panels.className = "codetabs-panels";

    langs.forEach(function (lang, i) {
      var id = "ct-" + Math.random().toString(36).slice(2, 8) + "-" + i;
      var btn = document.createElement("button");
      btn.type = "button";
      btn.className = "codetabs-tab";
      btn.textContent = lang;
      btn.setAttribute("role", "tab");
      btn.setAttribute("data-lang", lang);
      btn.setAttribute("aria-controls", id);
      btn.id = id + "-tab";
      bar.appendChild(btn);

      var panel = document.createElement("div");
      panel.className = "codetabs-panel";
      panel.id = id;
      panel.setAttribute("role", "tabpanel");
      panel.setAttribute("aria-labelledby", id + "-tab");
      panel.appendChild(blocks[i]);
      panels.appendChild(panel);
    });

    marker.parentNode.insertBefore(wrap, marker);
    wrap.appendChild(bar);
    wrap.appendChild(panels);
    marker.parentNode.removeChild(marker);
    wrap._langs = langs;

    bar.addEventListener("click", function (e) {
      var t = e.target.closest ? e.target.closest(".codetabs-tab") : null;
      if (!t) return;
      var lang = t.getAttribute("data-lang");
      selectAll(lang);
      remember(lang);
    });
    bar.addEventListener("keydown", function (e) {
      if (e.key !== "ArrowRight" && e.key !== "ArrowLeft") return;
      var tabs = Array.prototype.slice.call(bar.querySelectorAll(".codetabs-tab"));
      var idx = tabs.indexOf(document.activeElement);
      if (idx < 0) return;
      e.preventDefault();
      var n = e.key === "ArrowRight"
        ? (idx + 1) % tabs.length
        : (idx - 1 + tabs.length) % tabs.length;
      tabs[n].focus();
      var lang = tabs[n].getAttribute("data-lang");
      selectAll(lang);
      remember(lang);
    });
    return wrap;
  }

  function remember(lang) { try { localStorage.setItem(KEY, lang); } catch (_) {} }

  function selectGroup(wrap, lang) {
    var chosen = wrap._langs.indexOf(lang);
    if (chosen < 0) chosen = 0;
    var tabs = wrap.querySelectorAll(".codetabs-tab");
    var panels = wrap.querySelectorAll(".codetabs-panel");
    for (var i = 0; i < tabs.length; i++) {
      var on = (i === chosen);
      tabs[i].setAttribute("aria-selected", on ? "true" : "false");
      tabs[i].tabIndex = on ? 0 : -1;
      tabs[i].classList.toggle("is-active", on);
      panels[i].hidden = !on;
    }
  }
  function selectAll(lang) { groups.forEach(function (w) { selectGroup(w, lang); }); }

  document.addEventListener("DOMContentLoaded", function () {
    var markers = Array.prototype.slice.call(document.querySelectorAll(".codetabs"));
    markers.forEach(function (m) { var w = build(m); if (w) groups.push(w); });
    if (!groups.length) return;
    var saved = null;
    try { saved = localStorage.getItem(KEY); } catch (_) {}
    var def = saved || groups[0]._langs[0];
    selectAll(def);
  });
})();
