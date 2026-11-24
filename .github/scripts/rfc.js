'use strict';

// Sets the rfc-moratorium commit status on open pull requests and rewrites
// the pinned "Open RFCs" issue. Run by .github/workflows/rfc.yml; see RFC.md.

const fs = require('fs');
const path = require('path');

const LABEL = 'rfc';
const CONTEXT = 'rfc-moratorium';
const WINDOW_MS = 7 * 24 * 60 * 60 * 1000;
const INDEX_TITLE = 'Open RFCs';

function stamp(ms) {
    return new Date(ms).toISOString().slice(0, 16).replace('T', ' ') + ' UTC';
}

// When the rfc label was last applied, from the issue events, or null.
function labeledAt(events) {
    let at = null;
    for (const e of events) {
        if (e.event !== 'labeled' || !e.label || e.label.name !== LABEL) continue;
        const t = Date.parse(e.created_at);
        if (at === null || t > at) at = t;
    }
    return at;
}

function verdict(labels, events, now) {
    if (!labels.includes(LABEL)) return { state: 'success', description: 'Not an RFC' };
    const at = labeledAt(events);
    if (at === null) return { state: 'failure', description: 'Labeled rfc, but no labeled event found' };
    const notBefore = at + WINDOW_MS;
    if (now < notBefore) {
        return { state: 'failure', description: `RFC window open, merge not before ${stamp(notBefore)}`, notBefore };
    }
    return { state: 'success', description: `RFC window closed ${stamp(notBefore)}`, notBefore };
}

function parseStakeholders(text) {
    const rules = [];
    for (const raw of text.split('\n')) {
        const line = raw.replace(/#.*/, '').trim();
        if (!line) continue;
        const [pattern, ...handles] = line.split(/\s+/);
        rules.push({ pattern, handles: handles.map((h) => h.replace(/^@/, '')) });
    }
    return rules;
}

function stakeholders(rules, files, author) {
    const out = new Set();
    for (const { pattern, handles } of rules) {
        const hit = files.some((f) =>
            pattern === '*' || (pattern.endsWith('/') ? f.startsWith(pattern) : f === pattern));
        if (hit) handles.forEach((h) => out.add(h));
    }
    out.delete(author);
    return [...out];
}

function readStakeholders() {
    return fs.readFileSync(path.join(__dirname, '..', 'rfc-stakeholders'), 'utf8');
}

function indexBody(owner, repo, rows) {
    const head = 'Pull requests carrying the `rfc` label and the earliest time each can merge. ' +
        `The RFC workflow rewrites this issue, see [RFC.md](https://github.com/${owner}/${repo}/blob/main/RFC.md). ` +
        'Comment on the pull request itself.\n\n';
    if (rows.length === 0) return head + 'No open RFCs.\n';
    rows.sort((a, b) => a.notBefore - b.notBefore);
    return head + '| Pull request | Merge not before |\n|---|---|\n' +
        rows.map((r) => `| #${r.number} ${r.title.replace(/\|/g, '\\|').replace(/@/g, '&#64;')} | ${r.notBefore ? stamp(r.notBefore) : 'unknown'} |`).join('\n') + '\n';
}

// Pull requests can share a head commit, and a commit has one status per
// context, so a failing verdict for any of them wins.
function worst(a, b) {
    return !a || (b.state === 'failure' && a.state !== 'failure') ? b : a;
}

async function evaluate(github, owner, repo, prs, now) {
    const rows = [];
    const bySha = new Map();
    for (const pr of prs) {
        const labels = pr.labels.map((l) => l.name);
        const isRfc = labels.includes(LABEL);
        const events = isRfc
            ? await github.paginate(github.rest.issues.listEvents, { owner, repo, issue_number: pr.number, per_page: 100 })
            : [];
        const v = verdict(labels, events, now);
        if (isRfc) rows.push({ number: pr.number, title: pr.title, notBefore: v.notBefore });
        bySha.set(pr.head.sha, worst(bySha.get(pr.head.sha), v));
    }
    return { rows, bySha };
}

async function publishStatuses(github, core, owner, repo, bySha, target) {
    for (const [sha, v] of bySha) {
        const { data: current } = await github.rest.repos.listCommitStatusesForRef({ owner, repo, ref: sha, per_page: 100 });
        const last = current.find((s) => s.context === CONTEXT);
        if (last && last.state === v.state && last.description === v.description) continue;
        await github.rest.repos.createCommitStatus({
            owner, repo, sha, state: v.state, context: CONTEXT, description: v.description, target_url: target,
        });
        core.info(`${sha.slice(0, 7)}: ${v.state}, ${v.description}`);
    }
}

async function announce(github, owner, repo, pr, rows, target, stakeholdersText) {
    const files = (await github.paginate(github.rest.pulls.listFiles, { owner, repo, pull_number: pr.number, per_page: 100 }))
        .map((f) => f.filename);
    const text = stakeholdersText !== undefined ? stakeholdersText : readStakeholders();
    const who = stakeholders(parseStakeholders(text), files, pr.user.login);
    const row = rows.find((r) => r.number === pr.number);
    const when = row && row.notBefore ? stamp(row.notBefore) : 'the window closes';
    const body = `This is now an RFC: it cannot merge before ${when}, ` +
        'so anyone who depends on what it changes has a week to comment. See [RFC.md](' + target + ').' +
        (who.length ? '\n\n' + who.map((h) => '@' + h).join(' ') + ', this touches code you depend on.' : '');
    await github.rest.issues.createComment({ owner, repo, issue_number: pr.number, body });
}

async function updateIndex(github, core, owner, repo, rows) {
    const issues = await github.paginate(github.rest.issues.listForRepo, { owner, repo, state: 'open', per_page: 100 });
    const index = issues.find((i) => !i.pull_request && i.title === INDEX_TITLE);
    const body = indexBody(owner, repo, rows);
    if (!index) {
        core.warning(`No open issue titled "${INDEX_TITLE}", so no index to update.`);
    } else if (index.body !== body) {
        await github.rest.issues.update({ owner, repo, issue_number: index.number, body });
    }
}

async function run({ github, context, core, now = Date.now(), stakeholdersText }) {
    const { owner, repo } = context.repo;
    const target = `https://github.com/${owner}/${repo}/blob/main/RFC.md`;
    const prs = await github.paginate(github.rest.pulls.list, { owner, repo, state: 'open', per_page: 100 });
    const { rows, bySha } = await evaluate(github, owner, repo, prs, now);
    const payload = context.payload;
    // A pull request event runs in that pull request's concurrency group, so
    // it only writes the status of its own head commit. Statuses on other
    // pull requests are left to the serialized hourly run (see rfc.yml).
    const own = context.eventName === 'pull_request_target'
        ? new Map([...bySha].filter(([sha]) => sha === payload.pull_request.head.sha))
        : bySha;
    await publishStatuses(github, core, owner, repo, own, target);
    if (context.eventName === 'pull_request_target' && payload.action === 'labeled' && payload.label.name === LABEL) {
        await announce(github, owner, repo, payload.pull_request, rows, target, stakeholdersText);
    }
    await updateIndex(github, core, owner, repo, rows);
}

module.exports = { run, verdict, labeledAt, parseStakeholders, readStakeholders, stakeholders, indexBody, WINDOW_MS };
