'use strict';

// node --test .github/scripts/rfc.test.js ; run by .github/workflows/rfc-script.yml.

const test = require('node:test');
const assert = require('node:assert');
const rfc = require('./rfc.js');

const DAY = 24 * 60 * 60 * 1000;
const t0 = Date.parse('2026-09-01T12:00:00Z');
const on = (at) => ({ event: 'labeled', label: { name: 'rfc' }, created_at: new Date(at).toISOString() });
const off = (at) => ({ event: 'unlabeled', label: { name: 'rfc' }, created_at: new Date(at).toISOString() });

test('no label passes', () => {
    assert.strictEqual(rfc.verdict(['bug'], [on(t0)], t0).state, 'success');
});

test('fails inside the window, passes after it', () => {
    assert.strictEqual(rfc.verdict(['rfc'], [on(t0)], t0 + 7 * DAY - 1).state, 'failure');
    assert.strictEqual(rfc.verdict(['rfc'], [on(t0)], t0 + 7 * DAY).state, 'success');
});

test('re-applying the label restarts the clock', () => {
    const events = [on(t0), off(t0 + 6 * DAY), on(t0 + 6 * DAY + 1)];
    const v = rfc.verdict(['rfc'], events, t0 + 8 * DAY);
    assert.strictEqual(v.state, 'failure');
    assert.strictEqual(v.notBefore, t0 + 13 * DAY + 1);
});

test('other labels do not start the clock', () => {
    const other = { event: 'labeled', label: { name: 'bug' }, created_at: new Date(t0 + 5 * DAY).toISOString() };
    assert.strictEqual(rfc.verdict(['rfc'], [on(t0), other], t0 + 7 * DAY).state, 'success');
});

test('label with no event fails', () => {
    assert.strictEqual(rfc.verdict(['rfc'], [], t0).state, 'failure');
});

test('description fits a commit status', () => {
    assert.ok(rfc.verdict(['rfc'], [on(t0)], t0).description.length <= 140);
});

test('stakeholders by path, author left out', () => {
    const rules = rfc.parseStakeholders('# c\n*  @a\nmonitor/ @b @a\nopenbios/x.s @c\n');
    assert.deepStrictEqual(rfc.stakeholders(rules, ['psyqo/x.h'], 'z'), ['a']);
    assert.deepStrictEqual(rfc.stakeholders(rules, ['monitor/monitor.c'], 'a'), ['b']);
    assert.deepStrictEqual(rfc.stakeholders(rules, ['openbios/x.s'], 'z').sort(), ['a', 'c']);
    assert.deepStrictEqual(rfc.stakeholders(rules, ['openbios/x.s.bak'], 'z'), ['a']);
});

test('the real stakeholders file parses', () => {
    for (const { pattern, handles } of rfc.parseStakeholders(rfc.readStakeholders())) {
        assert.ok(pattern.length > 0);
        assert.ok(handles.length > 0, `${pattern} names nobody`);
        handles.forEach((h) => assert.match(h, /^[A-Za-z0-9-]+$/));
    }
});

test('run sets its own status, comments on labeling, rewrites the index', async () => {
    const calls = [];
    const prs = [
        { number: 37, title: 'monitor: protocol v3', labels: [{ name: 'rfc' }], head: { sha: 'aaa' }, user: { login: 'rixnobis' } },
        { number: 38, title: 'fix', labels: [], head: { sha: 'bbb' }, user: { login: 'x' } },
    ];
    const lists = {
        pulls: prs,
        events: [on(t0)],
        files: [{ filename: 'monitor/monitor.c' }],
        issues: [{ number: 47, title: 'Open RFCs', body: 'old' }],
    };
    const github = {
        paginate: async (fn, args) => fn(args),
        rest: {
            pulls: { list: () => lists.pulls, listFiles: () => lists.files },
            issues: {
                listEvents: () => lists.events,
                listForRepo: () => lists.issues,
                createComment: async (a) => calls.push(['comment', a]),
                update: async (a) => calls.push(['update', a]),
                create: async (a) => calls.push(['create', a]),
            },
            repos: {
                listCommitStatusesForRef: async () => ({ data: [] }),
                createCommitStatus: async (a) => calls.push(['status', a]),
            },
        },
    };
    const context = {
        repo: { owner: 'o', repo: 'r' },
        eventName: 'pull_request_target',
        payload: { action: 'labeled', label: { name: 'rfc' }, pull_request: prs[0] },
    };
    const core = { info() {}, warning() {} };
    await rfc.run({ github, context, core, now: t0 + DAY, stakeholdersText: '* @nicolasnoble\nmonitor/ @spicyjpeg\n' });

    const statuses = calls.filter((c) => c[0] === 'status').map((c) => [c[1].sha, c[1].state]);
    assert.deepStrictEqual(statuses, [['aaa', 'failure']]);
    const comment = calls.find((c) => c[0] === 'comment')[1];
    assert.match(comment.body, /2026-09-08 12:00 UTC/);
    assert.match(comment.body, /@nicolasnoble @spicyjpeg/);
    const update = calls.find((c) => c[0] === 'update')[1];
    assert.strictEqual(update.issue_number, 47);
    assert.match(update.body, /\| #37 monitor: protocol v3 \| 2026-09-08 12:00 UTC \|/);
});

test('index escapes mentions in titles', () => {
    const body = rfc.indexBody('o', 'r', [{ number: 1, title: 'ping @someone | x', notBefore: 0 }]);
    assert.ok(!body.includes('@someone'));
    assert.match(body, /&#64;someone \\\| x/);
});

test('a shared head commit gets the failing verdict', async () => {
    const statuses = [];
    const prs = [
        { number: 1, title: 'a', labels: [{ name: 'rfc' }], head: { sha: 'same' }, user: { login: 'x' } },
        { number: 2, title: 'b', labels: [], head: { sha: 'same' }, user: { login: 'x' } },
    ];
    const github = {
        paginate: async (fn, args) => fn(args),
        rest: {
            pulls: { list: () => prs },
            issues: { listEvents: () => [on(t0)], listForRepo: () => [] },
            repos: {
                listCommitStatusesForRef: async () => ({ data: [] }),
                createCommitStatus: async (a) => statuses.push([a.sha, a.state]),
            },
        },
    };
    const context = { repo: { owner: 'o', repo: 'r' }, eventName: 'schedule', payload: {} };
    await rfc.run({ github, context, core: { info() {}, warning() {} }, now: t0 + DAY });
    assert.deepStrictEqual(statuses, [['same', 'failure']]);
});
