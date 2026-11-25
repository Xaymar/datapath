// DataPath website: theme toggle, tabs, copy buttons. Everything works without it.
(function () {
	var root = document.documentElement;
	root.classList.add("js");

	// Theme: follows the OS until toggled, then remembered.
	var toggle = document.querySelector(".theme");
	if (toggle) toggle.addEventListener("click", function () {
		var dark = root.dataset.theme ? root.dataset.theme === "dark"
			: matchMedia("(prefers-color-scheme: dark)").matches;
		root.dataset.theme = dark ? "light" : "dark";
		try { localStorage.setItem("theme", root.dataset.theme); } catch (e) {}
	});

	// Graph tabs on the home page.
	document.querySelectorAll(".graph-card").forEach(function (card) {
		var tabs = card.querySelectorAll(".tab");
		tabs.forEach(function (tab) {
			tab.addEventListener("click", function () {
				tabs.forEach(function (t) {
					var on = t === tab;
					t.setAttribute("aria-selected", on);
					document.getElementById(t.getAttribute("aria-controls")).hidden = !on;
				});
			});
		});
	});

	// Language tabs on the getting-started page; #cpp, #rust ... select one.
	var langTabs = document.querySelectorAll(".langs .tab");
	if (langTabs.length) {
		var sections = document.querySelectorAll(".lang");
		var select = function (lang, remember) {
			var found = false;
			sections.forEach(function (s) { var on = s.dataset.lang === lang; s.classList.toggle("active", on); found = found || on; });
			if (!found) return false;
			langTabs.forEach(function (t) { t.setAttribute("aria-selected", t.dataset.lang === lang); });
			if (remember) try { localStorage.setItem("lang", lang); } catch (e) {}
			return true;
		};
		langTabs.forEach(function (t) {
			t.addEventListener("click", function () {
				select(t.dataset.lang, true);
				history.replaceState(null, "", "#" + t.dataset.lang);
			});
		});
		var initial = location.hash.slice(1), saved = null;
		try { saved = localStorage.getItem("lang"); } catch (e) {}
		if (!select(initial, false) && !(saved && select(saved, false))) select(langTabs[0].dataset.lang, false);
	}

	// Copy buttons on code blocks.
	if (navigator.clipboard) document.querySelectorAll("pre.code").forEach(function (pre) {
		var b = document.createElement("button");
		b.type = "button"; b.className = "copy"; b.textContent = "Copy";
		b.addEventListener("click", function () {
			navigator.clipboard.writeText(pre.querySelector("code").innerText).then(function () {
				b.textContent = "Copied";
				setTimeout(function () { b.textContent = "Copy"; }, 1500);
			});
		});
		pre.appendChild(b);
	});
})();
