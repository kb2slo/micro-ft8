// Text fields across the app (docs/architecture/webapp.md, Fields). Every text input says what a tap
// does with data-select: "all" selects its whole value, so the first key typed replaces it (a number,
// or a short code replaced whole); "off:<why>" leaves the caret where the tap put it (prose edited in
// place, a field that starts empty, a secret). webapp/tests/test_fields.js fails on an input with
// neither, and on type=number, which no field uses: a numeric keypad comes from inputmode instead.
//
// Selecting on a tap: iOS Safari ignores select() from a focus handler and puts the caret at the end
// (WebKit bug 193040), so the selection is set again just after the tap, from focus and from click; a
// desktop's mouseup would clear it, so the first mouseup after focus is cancelled. A second tap on a
// focused field places the caret as usual.
(function () {
  function selectAll(el) {
    try { el.setSelectionRange(0, el.value.length); } catch (e) { el.select(); }
  }
  function arm(el) {
    var fresh = false;
    el.addEventListener('focus', function () {
      fresh = true;
      setTimeout(function () { if (document.activeElement === el) selectAll(el); }, 0);
    });
    el.addEventListener('mouseup', function (e) { if (fresh) e.preventDefault(); });
    el.addEventListener('click', function () {
      if (!fresh) return;
      fresh = false;
      setTimeout(function () { if (document.activeElement === el) selectAll(el); }, 0);
    });
    el.addEventListener('blur', function () { fresh = false; });
  }
  function init() {
    document.querySelectorAll('input[data-select=all]').forEach(arm);
  }
  if (document.readyState === 'loading') document.addEventListener('DOMContentLoaded', init);
  else init();
})();
