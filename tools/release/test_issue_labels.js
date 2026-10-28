// Host test for public/.github/workflows/issue-labels.yml: runs the rules block of the workflow on sample issues.
// Usage: node tools/release/test_issue_labels.js   (SKIP, exit 0, when the workflow file is missing)
const fs = require('fs');
const path = require('path');
const file = path.join(__dirname, '..', '..', 'public', '.github', 'workflows', 'issue-labels.yml');
if (!fs.existsSync(file)) { console.log('SKIP: workflow not found'); process.exit(0); }
const text = fs.readFileSync(file, 'utf8');
const m = text.match(/\/\/ BEGIN rules[^\n]*\n([\s\S]*?)\/\/ END rules/);
if (!m) { console.log('FAIL: rules block not found'); process.exit(1); }
const { typeLabelFor, ALLOWED } = new Function(m[1] + '\nreturn { typeLabelFor, ALLOWED };')();
const form = (h) => `### ${h}\n\nsome text\n\n### Port build\n\nabc`;
const cases = [
  ['x', form('What happened, and what did you expect?'), 'bug'],
  ['x', form('What would you like, and why?'), 'enhancement'],
  ['x', form('Your feedback or question'), 'feedback'],
  ['x', form('What you see'), 'performance'],
  ['x', form('Template file (zipped)'), 'pipeline-template'],
  ['x', form('Template file (zipped)').replace(/\n/g, '\r\n'), 'pipeline-template'],
  ['[Bug] crash', 'free text', 'bug'],
  ['Feature request: widescreen', '', 'enhancement'],
  ['Question about saves', null, 'feedback'],
  ['Performance: slow', undefined, 'performance'],
  ['hello', 'no headings here', null],
  ['hello', 'text ### What you see in the middle of a line', null],
  ['A bugfix idea', '', null],
  [undefined, undefined, null],
];
let bad = 0;
for (const [t, b, want] of cases) {
  const got = typeLabelFor(t, b);
  if (got !== want) { bad++; console.log('FAIL', JSON.stringify(t), '->', got, 'want', want); }
  if (got !== null && !ALLOWED.includes(got)) { bad++; console.log('FAIL not in ALLOWED:', got); }
}
// Security shape of the workflow file.
const must = [[/permissions:\n  issues: write\n/, 'issues: write only'], [/actions\/github-script@[0-9a-f]{40}/, 'SHA-pinned']];
const mustNot = [[/pull_request_target/, 'pull_request_target'], [/actions\/checkout/, 'checkout'], [/\bsteps:[\s\S]*\brun:/, 'run: step'],
  [/removeLabel|setLabels|createLabel|ai-assisted/, 'forbidden call or label']];
for (const [re, name] of must) if (!re.test(text)) { bad++; console.log('FAIL missing:', name); }
for (const [re, name] of mustNot) if (re.test(text)) { bad++; console.log('FAIL present:', name); }
console.log(bad ? `FAIL (${bad})` : `PASS (${cases.length} cases + shape checks)`);
process.exit(bad ? 1 : 0);
