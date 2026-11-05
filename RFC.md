# RFCs

Some changes commit PCSX-Redux to something its users build on or rely on, and undoing them
later means breaking those users. A pull request making such a change gets the `rfc` label and
cannot merge for seven days, so the people who depend on it can comment first.

## What needs one

- Save state changes that stop older states from loading. A new protobuf field that older
  states simply load as its default value is not one of these.
- New UX: new windows, menus or workflows, changed defaults, removed options.
- The Lua API.
- Command-line flags.
- The GDB server and web server protocols.
- File formats.

A bugfix that makes the code do what it already promises does not need one, and neither do
tests or documentation. Who opens the pull request makes no difference. `src/mips` is the
[nugget](https://github.com/pcsx-redux/nugget) submodule, which runs its own RFCs; bumping it
here is not one. If you are not sure, ask on the pull request or add the label.

## The window

While the label is on, the `rfc-moratorium` check fails until seven days after the label was
last applied, and `main` requires that check. The check's description gives the time the
window closes. If the proposal changes during the window, remove and re-apply the label and say
what changed in a comment; the seven days start over. Changes that leave the proposal alone do
not restart it.

Open RFCs are the
[open pull requests with the label](https://github.com/pcsx-redux/pcsx-redux/pulls?q=is%3Apr+is%3Aopen+label%3Arfc).
When the label goes on, the people listed in [.github/rfc-stakeholders](.github/rfc-stakeholders)
are mentioned on the pull request.

## What an RFC has to say

Before the window closes, the pull request description states what the change costs and who
it affects: for a save state change, which versions stop loading; for a UX change, what a user
does differently afterwards, with a screenshot; for anything else, the size and speed it costs,
measured on a build. An RFC missing that does not merge when the window closes; it waits until
it is there.

## Commenting

Comment on the pull request. An objection helps most with a use case attached: what you do
today that the change would break, or what it would stop you from doing.
