#!/usr/bin/env node
// The marks on decodes and the filter on them (docs/architecture/webapp.md, Worked and activation cues on
// decodes), in one browser on a simulated station: before its QSO, N6HAN's decodes read New; once the simulator
// logs the QSO, its next decode reads Dupe. The Filter sheet's marks row hides it under No dupes, its pill
// says so, and All brings it back.
'use strict';
const { run, check, until } = require('./harness');

const PARTNER = 'N6HAN';  // host_mock/test_qso.json: the QSO the simulator completes and logs

run(async (st) => {
  const A = await st.browser('A');
  await A.evaluate(() => showView('operate'));
  const markOf = (re) => A.evaluate((src) => {
    const re = new RegExp(src);
    return [...document.querySelectorAll('#opDecodeRows .wflag')].filter((f) => {
      const row = f.closest('.drow') || f.parentElement.parentElement;
      return re.test(row.textContent);
    }).map((f) => f.textContent);
  }, re.source);
  const sawNew = await until(async () => (await markOf(new RegExp('CQ ' + PARTNER))).includes('New'), 60000);
  check('before the QSO, the partner\'s CQ reads New', sawNew, await markOf(new RegExp('CQ ' + PARTNER)));
  const logged = await until(async () => (await (await fetch(st.origin + '/api/log/entries?take=1')).json()).total >= 1, 90000);
  check('the simulator logs its QSO', logged);
  const dupe = await until(async () => (await A.evaluate(() => decodeSlots.some((s) => s.some((d) => (d.wk & 1) && / N6HAN /.test(' ' + d.x + ' '))))), 60000);
  check('after it, a decode from the partner carries the dupe bit', dupe);
  const shown = await until(async () => (await markOf(new RegExp(PARTNER))).includes('Dupe'), 10000);
  check('and reads Dupe', shown, await markOf(new RegExp(PARTNER)));

  // No dupes hides it; All brings it back.
  const pick = async (need) => {
    await A.click('#dvFilter');
    await A.click('#dvFilterNeed [data-need=' + need + ']');
    const note = await A.textContent('#dvNeedNote');
    await A.click('#dvFilterSheet [data-act=apply]');
    return note;
  };
  const note = await pick('nodupes');
  check('the sheet explains No dupes', /worked today on this band/.test(note), note);
  check('the pill reads No dupes', (await A.textContent('#dvFilter')) === 'No dupes', await A.textContent('#dvFilter'));
  const gone = await A.evaluate(() => ![...document.querySelectorAll('#opDecodeRows .wflag-dupe')].length);
  check('no row marked Dupe shows', gone);
  await pick('all');
  check('All: the pill is plain Filter again', (await A.textContent('#dvFilter')) === 'Filter', await A.textContent('#dvFilter'));
  check('and the Dupe row is back', await until(async () => (await A.$$('#opDecodeRows .wflag-dupe')).length > 0, 5000));
});
