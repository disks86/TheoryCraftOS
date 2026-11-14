# User interface design guidelines

These are the rules for every user interface in this project: the desktop shell, the built-in applications, dialogs, and any new control
added to the toolkit in `src/Gui`. They describe a classic desktop: overlapping frames with title bars, menus, dialogs, a pointer and a
keyboard. Follow them when writing or reviewing interface code. Where a rule cannot be followed, deviate in its spirit and say why in the
change.

How to read this document:

- **Must / never** rules are requirements. **Prefer / avoid** rules are defaults that a good reason can override.
- Sizes are in logical pixels at 100% display scale unless they say "units" (see [Layout](#12-layout)). Never hard-code physical pixels:
  the desktop runs at any scale.
- Key names are written `Ctrl+C` (hold the first, press the second) and `Alt, F` (press one after the other).
- "Primary button" is the left mouse button and "secondary button" the right one, unless the user has swapped them.
- [Section 18](#18-review-checklist) is a checklist to run before a change is finished.

Contents: [1 Principles](#1-principles) · [2 Objects](#2-think-in-objects-not-programs) · [3 Pointer input](#3-pointer-input) ·
[4 Keyboard](#4-keyboard-input) · [5 Selection](#5-selection) · [6 Editing and transfer](#6-editing-undo-and-transfer) ·
[7 Frames](#7-frames) · [8 Menus](#8-menus) · [9 Controls](#9-controls) · [10 Toolbars and status bars](#10-toolbars-and-status-bars) ·
[11 Secondary frames](#11-secondary-frames-dialogs-property-sheets-messages) · [12 Layout](#12-layout) · [13 Visual design](#13-visual-design) ·
[14 Text](#14-text-and-wording) · [15 Help](#15-user-assistance) · [16 System integration](#16-fitting-into-the-system) ·
[17 Accessibility, sound, languages](#17-accessibility-sound-and-other-languages) · [18 Checklist](#18-review-checklist) ·
[19 Toolkit map](#19-how-this-maps-to-the-toolkit)

## 1. Principles

Seven principles decide every case the detailed rules do not cover.

1. **The user is in control.** The user starts actions; the software responds. Do not take focus, move things, or start work the user did
   not ask for. Let the user customize (colours, fonts, which bars are shown) and honour the system settings for those.
   Avoid modes. Where a mode is the best design (a drawing tool), it must be visible, the result of an explicit choice, and easy to leave.
2. **Directness.** Let the user act on a visible representation of the thing itself, and show the effect on it. Make choices visible so
   the user recognises them instead of recalling them. A metaphor is a bridge to understanding, not a limit: a folder may hold things a
   paper folder cannot.
3. **Consistency.** The same function has the same name, the same place and the same behaviour everywhere: inside one application, across
   applications, and with the metaphor in use. Never change what a standard control or standard shortcut does.
4. **Forgiveness.** People explore. Offer only choices that apply, make actions reversible, and where an action cannot be reversed, ask
   first. Design so the likely mistake cannot happen (disable the command) before designing the message that explains it.
5. **Feedback.** Every action gets an immediate visible response, close to where the user acted. While work is in progress show that it
   is, how far along it is, and how to cancel it. A frame must never look dead for more than a moment. Match the weight of the feedback
   to the event: a pointer change or status text for small things, a message only for things that need a decision.
6. **Aesthetics.** Every visual element competes for attention. Add one only if it tells the user something.
7. **Simplicity.** Show the minimum needed and group related things. Use progressive disclosure: common choices first, detail on demand.
   Simple is not crippled: all functionality stays reachable. Never make a modifier key, a double-click or a drag the only way to do a
   basic task.

Design for these users at the same time:

- **Beginners** find dragging and double-clicking hard, assume a covered frame is gone, and get lost in folders nested more than two deep.
  Every basic task must be possible with single clicks and visible commands.
- **Experienced users** want shortcuts. Add them without adding steps for anyone else, and do not rearrange what they already know.
- **Users with disabilities and users of other languages** are planned for from the start, not added later
  ([section 17](#17-accessibility-sound-and-other-languages)).

Every feature costs performance, complexity and stability. A small extension that complicates the common case (multiple selection where
single selection is the norm) needs to earn its place.

## 2. Think in objects, not programs

- The user works on **objects**: a file, a paragraph, a cell, a shape, a device. Each object has **properties** (its attributes and
  state), **operations** (commands and direct manipulation), and **relationships** to other objects (collection, containment,
  composition, constraint).
- The interaction order is always **object, then action**: select something, then choose what to do with it. Commands apply to the
  selection and are offered only when they apply.
- An object's settings are reached through one **Properties** command that opens a property sheet. Do not scatter them over "Options",
  "Info" and "Format" commands.
- The same conventions hold at every level of nesting. Selecting, opening and dragging work the same for a file in a folder and a shape
  in a drawing.
- **State persists.** Reopen things the way the user left them: frame size and position, scroll position, selection, view type, the last
  page of a dialog, the last folder of a file dialog. Consider saving data automatically instead of asking.
- Users must be able to create an object without first finding the program for it: by copying an existing one, by a **New** command in
  the container, or from a template.

## 3. Pointer input

### 3.1 Actions

| Action | Meaning |
| --- | --- |
| Point | Move over something without pressing. May show feedback (pointer shape, tooltip). Never triggers a command. |
| Press | Button down. Identifies the target and shows pressed feedback. Does not carry out the operation. |
| Click | Press and release in place. Primary button: select or activate. Secondary button: select and show the context menu. |
| Double-click | Primary button: the default command of the object (usually Open). It is always a shortcut for a visible command. |
| Drag | Press, move, release. Primary button: select a range, or move with the default transfer. Secondary button: transfer with a menu of choices at the drop. |

- The operation happens on **release**, with the pointer still over the control. Moving off before releasing cancels; moving back resumes.
  The exception is auto-repeat (scroll arrows, spin buttons), which acts while held.
- Do not use chords (two buttons at once) or triple-clicks for basic operations.
- The middle button and extra buttons have no basic meaning. Use them only for a redundant shortcut.
- The wheel scrolls the scrollable area under the pointer.

### 3.2 Pointer shapes

Change the pointer only to say something about what a click here would do. Use the toolkit's shapes with these meanings and no others:

| Shape | Meaning |
| --- | --- |
| Arrow | Normal pointing, selecting, moving. Also the default while dragging. |
| I-beam | Over editable or selectable text. |
| Wait | This frame cannot take input right now. Show it only over that frame. |
| Resize (horizontal, vertical, two diagonals) | Over a sizing border, a column divider or a split bar. |
| Move | Over a move handle, or during a move started from a command. |
| Crosshair | Precise selection or drawing. |
| Hand | A click here does something other than select (a link, a jump). |
| No | Dropping here is not possible. |

- The hot spot must be obvious from the picture (the tip of the arrow, the centre of the crosshair).
- Do not flicker through several shapes as the pointer crosses the screen. Delay non-essential changes briefly.
- For anything longer than a few seconds show a progress indicator, not only the wait pointer. For background work keep the normal
  pointer so the user knows the foreground still responds.

### 3.3 Touch and pen

A tap is a primary click, a double tap a double-click, and a drag a drag. Touch has no hover and often no secondary button, so nothing
may depend on hovering, and the context menu needs another route (a menu button, a toolbar button or a long-standing command in a menu).
Do not use timing ("hold still for a second") as the only way to tell two operations apart.

## 4. Keyboard input

**Everything must be usable from the keyboard alone.** The pointer is never the only route to a basic function.

- Key actions happen on key down. Both Enter keys do the same thing.
- A shortcut is never the only way to reach a command; the command is also in a menu or on a button.
- `Esc` stops the operation in progress, cancels a drag, closes a menu, and is the Cancel button of a dialog.
- Modifiers are held, not locked. Avoid needing two modifiers for a common command.

### 4.1 Access keys

An access key is the underlined letter in a menu title, menu item or control label; `Alt+letter` moves to the control and activates it.

- Every menu title, menu item and labelled control gets one. They are unique within their scope (one menu, one dialog).
- Choose in this order: the first letter, else a distinctive consonant, else a vowel. A static label gives its access key to the control
  that follows it.
- Use the same access key for the same command everywhere it appears.
- `OK` and `Cancel` have no access key: `Enter` and `Esc` are their keys.

### 4.2 Shortcut keys

- Prefer `Ctrl+letter` and function keys. `Shift` extends or reverses the unshifted key (`Shift+Tab` goes back). `Ctrl` enlarges the
  effect (`Home` is the start of the line, `Ctrl+Home` the start of the data).
- Avoid `Alt+letter` (it belongs to access keys), `Shift+letter`, punctuation keys, and `Ctrl+Alt+key` (it is a typing key on many
  keyboard layouts).
- The system key (Super) and every combination with it belong to the desktop shell. Applications never use it.
- Never give these keys another meaning:

| Key | Command |
| --- | --- |
| `Ctrl+X`, `Ctrl+C`, `Ctrl+V` | Cut, Copy, Paste |
| `Ctrl+Z` | Undo |
| `Ctrl+O`, `Ctrl+S`, `Ctrl+P` | Open, Save, Print |
| `Del` | Delete the selection |
| `F1` | Help for the thing that has focus |
| `Shift+F1` | Context help mode (the next click explains what was clicked) |
| `Shift+F10` and the Menu key | Context menu for the selection |
| `Space` | Select or toggle the focused item (`Ctrl+Space` where Space types) |
| `Enter` | The default command or default button |
| `Alt+Enter` | Properties of the selection |
| `Esc` | Cancel |
| `Alt` | Enter or leave the menu bar |
| `Alt+Tab`, `Alt+Esc` (+`Shift` to reverse) | Next application frame, next frame |
| `Alt+F6` (+`Shift`) | Next frame of the same application (a modeless dialog and its owner) |
| `Ctrl+F6`, `Ctrl+Tab` (+`Shift`) | Next document or tab inside a frame |
| `Alt+Space` | The frame's own menu (Restore, Move, Size, Minimize, Maximize, Close) |
| `Alt+F4` | Close the frame |
| `Ctrl+Esc` | Open the launcher menu |

### 4.3 Navigation keys

| Key | Alone | With `Ctrl` |
| --- | --- | --- |
| `Left` / `Right` | One unit (character, item) | One larger unit (word) |
| `Up` / `Down` | One line or item | One larger unit (paragraph) |
| `Home` / `End` | Start / end of the line | Start / end of the data |
| `Page Up` / `Page Down` | One screenful, cursor keeps its place on the screen | One screenful sideways, or previous / next large unit |
| `Tab` | Next field (`Shift+Tab` previous) | Next page or pane |

Adding `Shift` to any of them extends the selection to the new position.

## 5. Selection

- **Always show what is selected**, as soon as the press happens, with the system selection colours or with handles.
- Show selection only in the active frame and pane. Hide it when the frame is deactivated and bring it back unchanged when the frame is
  activated again. Activating a frame never changes its selection.
- A selection is scoped to its container. Selecting in one list does not disturb another, and a command acts only inside the scope it
  was given in.

### 5.1 With the pointer

| Input | Result |
| --- | --- |
| Click an unselected item | It becomes the only selection and the anchor. |
| Click a selected item | The selection stays (so that it can be dragged). |
| Click empty space | The selection is cleared. |
| Drag from empty space | Region selection: everything the rectangle covers. Decide per view whether touching or enclosing counts and show which. |
| Drag from inside the selection | Transfers the selection (move by default). |
| `Shift+click` or `Shift+drag` | Extends the range from the anchor to the pointer. The anchor does not move. |
| `Ctrl+click` | Toggles that item and leaves the rest alone. It becomes the anchor. |
| `Ctrl+drag` from empty space | Toggles the items in the region, all to the state the first one changes to. |
| Secondary click on an unselected item | Selects it, then shows its context menu. |
| Secondary click on the selection | Keeps the selection and shows the menu for all of it. |
| Double-click | Selects, then runs the default command. In text, selects the word. |

The modifier must be down before the button. Once the drag starts it finishes the same way even if the modifier is released.

### 5.2 With the keyboard

- Navigation keys move the focus and, in single-selection views, select what they land on.
- `Shift+navigation` selects from the anchor to the new position. A plain navigation key then collapses the selection.
- For a disjoint selection, `Ctrl+navigation` moves the focus without selecting and `Space` toggles the focused item.
- In a list with nothing selected, the first navigation key selects the first item; it does not skip to the second.

### 5.3 Several items selected

- Offer the commands that apply to all of them (the intersection).
- A property sheet for several objects shows the properties they share; where the values differ the control shows the mixed-value state
  (see [9.1](#91-states-every-control-has)).
- A group is one object, not a multiple selection: show the group's properties.

## 6. Editing, undo and transfer

### 6.1 Text

- Typing inserts at the insertion point and replaces the selection if there is one. Pasting does the same.
- `Del` removes the character after the insertion point and `Backspace` the one before; with a selection both remove the selection.
  Deleted text does not go to the clipboard, so text editing needs at least one level of undo.
- `Insert` may toggle overtype; if it does, show the mode (a block cursor).
- Double-click selects a word with its trailing space but not its punctuation.

### 6.2 Handles

A selected graphic shows handles: small squares on its edges. A filled handle can be dragged (resize, reshape). A hollow handle only
shows that the object is selected and cannot be dragged. A title bar is the move handle of a frame; a selected icon is its own.

### 6.3 Committing and undoing

- Best: apply changes at once and offer **Undo** (and **Redo**). Where that is not practical, hold the changes as a pending transaction
  and commit explicitly.
- The commands have fixed meanings. Do not blur them:

| Command | Meaning |
| --- | --- |
| Save | Commit the document; keep working. |
| Apply | Commit pending changes; the dialog stays open. |
| OK | Commit pending changes and close. |
| Cancel | Discard pending changes, restore the previous state and close. Does not undo what Apply already committed. |
| Close | Remove the frame. Not the same as Cancel, and not the same as quitting the work behind it. |
| Stop | Halt a process where it is, with no promise of restoring anything. |
| Pause / Resume | Suspend and continue a process. |
| Undo / Redo / Repeat | Reverse the last action / restore it / do it again. |

- Closing with uncommitted changes always asks: save, discard, or cancel the close. Closing with nothing pending just closes.
- One undo history per frame. Everything done in the frame, whatever part of the content it touched, undoes in order.

### 6.4 Clipboard

- **Cut**, **Copy** and **Paste** are on the Edit menu and the context menu of every selection that supports them.
- Cut removes the selection at once (or shows it dimmed until it is pasted). Copy gives no feedback. Paste never changes the clipboard,
  so it can be repeated.
- Paste replaces the selection, or inserts at the insertion point and leaves it after the new content. In a free-form view the pasted
  object is placed at the pointer if Paste came from a context menu, otherwise at the next free place, and becomes the selection.
- The destination decides what form the data takes. When more than one form is useful, name it in the command (`Paste Picture`) or offer
  **Paste Special...** for the choice.

### 6.5 Drag and drop

Drag and drop is a shortcut for cut, copy and paste. It is never the only way to transfer something.

- **Default drag (primary button):** the destination decides the operation. It is usually Move inside one container and may be Copy,
  Link or Print elsewhere. The default must not be destructive; if it must be, confirm it.
- **Drag with a menu (secondary button):** on the drop the destination shows a menu: `Move Here`, `Copy Here`, `Link Here`, other
  operations it supports, a separator and `Cancel`. The operation a primary drop would have done is shown in bold.
- `Ctrl` during a drag makes it a copy. `Esc` cancels. Dropping on the source cancels.
- A drag that starts inside the selection transfers; one that starts on empty space selects.
- Any visible place can be a destination, including a frame that is not active. The frame is activated only when the drop happens.
  Dragging over a task-bar button brings that frame forward so the drop can be made into it.
- Feedback while dragging:
  - The source stays where it is, shown selected, until the drop completes.
  - A translucent or outline image of what is dragged follows the pointer. It shows what it will become at the destination (an icon, a
    block of text, an outline) and does not hide the insertion point.
  - The pointer is what the destination uses for insertion, with a badge at its lower right: none for move, a plus for copy, an arrow
    for link. Over a place that cannot accept the drop, the "No" pointer.
  - The destination highlights where the drop would land.
- **Autoscroll:** when the pointer rests in a narrow band along the edge of a scrollable area, scroll. Do not scroll when it passes
  through quickly, so the user can drag out of the area. Judge speed from the last few pointer positions (sample every 100 ms).

### 6.6 Deleting, naming, linking

- **Delete** removes an object from its container; files go to the trash, never straight to oblivion. **Clear** empties content without
  removing the container.
- A new object gets a name automatically from its type: `Document`, then `Document 2`. A copy is `Copy of Name`, then `Copy 2 of Name`.
  A link is `Link to Name`.
- A link stores both a relative and an absolute path to its source; use the relative one first and fall back to the other.
- A single click only selects. If a single click does something else (follows a link), show it with a button look or the hand pointer.

## 7. Frames

A primary frame is where the user views and edits one object. It has a border, a title bar, and optionally a menu bar, toolbars, scroll
bars and a status bar.

### 7.1 Title bar

- At the left is a small icon of the object being viewed: the document's type icon when the frame shows a document, the application's
  icon for a tool with no documents.
- The title text names that object exactly as the file explorer shows it. No path, and no extension unless the user asked for them.
- With an application name: `Document name - Application`. A tool with no documents puts its own name first, then any specifier
  (`Find - report*`). The rule: the text is in the same order as the icon implies.
- A new, unsaved document is titled with its type and a number (`Document 1`), and the same name is the default in Save As.
- Update the title immediately if the object is renamed.
- Buttons at the right: Minimize, Maximize (which becomes Restore), Close. Leave out a button whose command the frame does not support.
- Never draw in the title bar or put controls there.
- A secondary click on the title bar, or `Alt+Space`, shows the frame menu: Restore, Move, Size, Minimize, Maximize, Close. A secondary
  click on the title icon shows the object's own menu, with Close in place of Open; double-clicking the icon closes.
- An abbreviated title keeps the part that identifies the object.

### 7.2 Activating

- Exactly one frame is active. It has the active title colours and receives the keyboard. Clicking anywhere in a frame activates it.
- Activating a frame raises it together with its dialogs, in their existing order.
- A frame that opens goes on top and becomes active, **unless the user has started working elsewhere since asking for it**; then it opens
  behind. Never take focus from what the user is doing.

### 7.3 Opening and closing

- Opening an object that is already open activates its existing frame. One object, one frame.
- Reopen a frame where and how it was: size, position, view, scroll position, selection. The first time, choose a sensible default.
  Adjust a remembered position to the current screen so the frame is fully visible and reachable.
- Closing a primary frame closes its dialogs. Whether work in progress stops depends on the object: closing a print queue's frame does
  not cancel the jobs.
- Each open primary frame has an entry on the task bar. The entry stays while the frame is minimized and goes when it closes.
  Something that runs with no frame has no task-bar entry.

### 7.4 Moving and sizing

- Drag the title bar to move, drag the border to size. Both are also commands in the frame menu, driven by the arrow keys and committed
  with `Enter`.
- A frame can never be put where it cannot be reached again.
- Primary frames are resizable unless the content has one natural size (a calculator). Give a minimum size below which the frame stays
  usable. When made smaller, clip or re-flow; do not scale text down.
- Maximize fills the work area (the screen minus the task bar and other reserved edges). Never assume a screen size.
- A resizable frame shows a size grip in its lower-right corner: in the status bar if there is one, otherwise where the scroll bars meet.

### 7.5 Scrolling

- Scroll when the content is larger than the view. Leave out a scroll bar only for a direction that can never scroll; otherwise keep it
  and disable it when everything fits, so the layout does not jump.
- The scroll box shows position by where it is and proportion by how big it is. Dragging it scrolls continuously.
- An arrow scrolls one line or unit and auto-repeats while held. Disable the arrow at that end of the content.
- A click in the shaft scrolls one screenful, keeping a line of overlap so the user keeps their place.
- Scroll automatically when: a drag reaches the edge; typing or the cursor moves out of view; a command selects something out of view
  (Find, the next field in a form).
- Keyboard scrolling moves the cursor with it. Scroll bars themselves never take keyboard focus.
- Use a scroll bar only to scroll. To set a value use a slider or spin box.

### 7.6 Split views and several documents

- Split a frame into panes to show two parts or two views of the same object. A split bar between the panes is dragged to size them;
  dragging it to the edge removes the split. Offer a **Split** command for the keyboard. Each pane keeps its own scroll position; show
  the selection only in the active pane. Save the split with the rest of the view state.
- **One frame per document is the default.** The task bar is the list of open documents.
- For several documents in one frame, prefer **tabs**: one tab per document, the shared menu and toolbar following the active tab,
  `Ctrl+Tab` to switch, a close button on each tab, and the frame title following the active tab. A tab can be dragged out to become
  its own frame and dragged into another tabbed frame.
- Use child frames inside a parent frame only when the user must see several views side by side that share one set of tools. Then the
  parent needs a **Window** menu that lists the children and arranges them (Tile, Cascade); closing the parent closes all and reopening
  restores them.
- Whatever the model, shared menus and toolbars keep common commands in the same place as the active document changes.

## 8. Menus

Menus make commands visible. Three kinds: drop-down menus from the menu bar, context menus at the pointer, and cascading submenus.

### 8.1 Menu bar

- It sits directly under the title bar. Each title is one word with a unique access key.
- Standard menus, in this order when present: **File**, **Edit**, **View**, application menus, **Window**, **Help**.

| Menu | Holds |
| --- | --- |
| File | New, Open, Close, Save, Save As..., Print..., Send To, then a separator and **Exit** last. Use **Close** in place of Exit when closing the frame does not end the work behind it. |
| Edit | Undo, Repeat; Cut, Copy, Paste, Paste Special...; Delete, Duplicate, Select All; Find..., Replace... |
| View | Commands that change the view and not the data (Zoom, view type), and show/hide for toolbars, the status bar, rulers. |
| Window | Only for frames that hold several documents: the list of them and arrangement commands. |
| Help | Help Topics, then a separator and `About <Application>` last (no ellipsis). |

- `Alt` enters the menu bar. `Left`/`Right` move between titles and `Up`/`Down` through the items, both wrapping. `Enter` chooses. `Esc`
  backs out one level. Typing an item's access key chooses it.
- If the user can hide the menu bar, every command must still be reachable (context menus, toolbars).

### 8.2 Context menus

- **Every selectable object and every identifiable area has one**, shown by a secondary click, `Shift+F10` or the Menu key.
- It appears on release, to the right of and below the pointer, moved as needed to stay on the screen. A click elsewhere or `Esc`
  dismisses it.
- It holds only commands for that object in that context, and few of them. One **Properties** item, not a list of properties.
- Order: the object's primary commands (Open, Play, Print; the default one in bold), then transfer commands (Cut, Copy, Paste), then
  other commands, then **Properties** last.
- A context menu is never the only place a command exists.
- Do not show shortcut keys in context menus.
- The menu on a control acts on what the control stands for, not on the control: a scroll bar's menu says "Top", "Bottom", "Page Up".
- Empty space has a menu too, with the commands of the view or container (Paste, New, Arrange, Select All, Properties).

### 8.3 Cascading menus

A triangle at the right of an item marks a submenu. Use at most one level and never for frequent commands. Open and close them after a
short delay so the pointer can travel diagonally into the submenu.

### 8.4 Menu items

- Group related items and separate groups with a line.
- An item that does not apply now is **disabled, not removed**, so the menu keeps its shape. Remove an item only when it can never apply
  in this context. Never leave an item enabled and answer it with a message saying it cannot be used. If every item is disabled, disable
  the title too.
- **Ellipsis:** end the label with `...` when the command needs more information from the user before it can run (`Save As...`). Do not
  use one merely because a frame appears: `Properties`, `About` and commands that may show a confirmation have none.
- A toggle shows a check mark when on (`Bold`). One of a set shows a bullet on the chosen item (`Left`, `Centre`, `Right`). If the two
  states are not obvious opposites, use two items or an item whose name is the action it will perform.
- Do not change items when a modifier key is held; that hides the function.
- The default item (what double-click does) is bold.
- Shortcut keys are shown right-aligned in their own column, written as on the key caps: `Ctrl+S`, `F5`.
- Labels are short, unique within the menu, in title capitalization ([14.1](#141-capitalization)), each with a unique access key.

## 9. Controls

Use the toolkit's controls. Write a new one only when none fits, and then give it every state and every keyboard behaviour below.

### 9.1 States every control has

| State | Appearance |
| --- | --- |
| Normal | As designed. |
| Pressed | Shown the moment the button or key goes down. A command button sinks and its label moves one pixel down and right. |
| Set ("on") | A value that is on must look different from pressed: a check mark, a dot, a held-down button with a lighter face. |
| Mixed value | The selection has different values for this property: a blank field, a dimmed check. Clicking sets it, clicking again clears it, a third click returns to mixed (which changes nothing when applied). |
| Unavailable | The label is drawn in the disabled-text colour. The control still explains itself in context help. |
| Focus | Text fields show the insertion point. Every other control shows a dotted rectangle around itself or its label. |
| Flat | Inside a scrolling list or grid, controls are drawn flat, without bevels. |

### 9.2 Choosing a control

| The user needs to | Use |
| --- | --- |
| Start an action | Command button |
| Turn one independent thing on or off | Check box (toggle switch for an immediate on/off setting) |
| Pick one of 2 to 7 fixed choices | Radio buttons |
| Pick one of many, or of a changing set | List, or drop-down list where space is short |
| Pick several | List with a check box per item, or an extended-selection list |
| Pick one or type another | Combo box |
| Enter free text | Text box |
| Step through a small ordered range of numbers | Spin box |
| Set a point on a continuous range | Slider |
| See a hierarchy | Tree view |
| See objects with icons or in columns | List view |
| Switch between pages of related settings | Tabs |
| See how far a long operation has got | Progress bar |

### 9.3 Rules per control

**Command button.** The label is the action, in title capitalization, with `...` under the same rule as menu items. It acts at once.
`Space` presses the focused button. A button that opens a menu shows a triangle. A button that expands the dialog ends in `>>`.

**Radio buttons.** Always a group of two or more, at most about seven, with exactly one set. Selecting only sets the value; it starts
nothing. Clicking the label works like clicking the button. `Tab` reaches the set button; arrow keys move within the group.

**Check box.** Only for a choice whose two states are plain opposites. Clicking the label toggles it. `Space` toggles. Each one is a tab
stop. A check box may enable or disable other controls.

**Lists.** Order the items in a way the user can predict (alphabetical, numeric, by date). Leave unavailable choices out. Show 3 to 8
rows unless the list fills its frame. Fit the width to the typical entry; shorten longer entries with an ellipsis in the middle or at
the end. Typing letters jumps to the matching item. A list has no label of its own: put a static label above or to its left, whose
access key moves focus to the list. A drop-down list opens with a click, `Alt+Down` or `Down`, and closes when an item is chosen.

**List view and tree view.** Offer the standard views where they make sense: large icons, small icons, list, details with sortable
column headings. Clicking a column heading sorts by it and shows the direction; dragging a divider sizes the column. Items support the
same selection, context menus, renaming and drag and drop as files in a folder. In a tree, `Right` expands and `Left` collapses or goes
to the parent; show the expand/collapse buttons.

**Text box.** A static label names it. Reject invalid characters as they are typed where the field has a format. A read-only field
looks different (no sunken white field) but its text can still be selected and copied. Move focus on automatically after the last
character only in fixed-format data entry.

**Combo box.** Typing scrolls the list to the nearest match; choosing an item puts it in the field, selected.

**Spin box.** The upper button increases, the lower decreases; `Up` and `Down` do the same. The value can also be typed.

**Slider.** Label it and label its ends or ticks. Arrow keys move it by one step.

**Tabs.** One row by default. Labels are short, in sentence capitalization. `Ctrl+Tab` switches pages; `Left`/`Right` when a tab has
focus. A graphic-only tab needs a tooltip.

**Group box.** Frames related controls under a label. Labels inside are read relative to it (`Alignment`: `Left`, `Right`).

**Progress bar.** Fills from left to right. Use it for anything longer than a few seconds, with text saying what is happening and a
Cancel or Stop button where possible. Put it in the status bar for background work and in a dialog for work that blocks.

**Tooltip.** Names a control that has no text label. It appears after the pointer rests, at the lower right of the pointer, and goes on
a click or when the pointer leaves. Moving straight to the next control shows its tooltip without the delay.

## 10. Toolbars and status bars

- A toolbar gives quick access to commands that are **also** in the menus. Nothing lives only on a toolbar.
- Toolbar buttons are 24 wide by 22 high with a 16 by 16 picture; the large size is 32 by 30 with a 24 by 24 picture. At other display
  scales use pictures drawn for that scale or whole multiples, never fractional stretching.
- Every button without a text label has a tooltip.
- A button for a property (Bold) applies to the selection at once and shows the selection's current state, including mixed.
- Space between buttons is one border width; buttons that form a one-of-many set touch.
- Use the standard pictures only for their standard meanings: New, Open, Save, Print, Print Preview, Undo, Redo, Cut, Copy, Paste,
  Delete, Find, Replace, Properties, Bold, Italic, Underline, Help, Up one level, and the four view types.
- Let the user show and hide each bar (View menu) and remember the choice.
- A status bar at the bottom shows the state of what is viewed, a one-line description of the menu item or toolbar button under the
  pointer, and progress of background work. It is read-only. **Nothing essential is only in the status bar**, because it can be hidden.

## 11. Secondary frames: dialogs, property sheets, messages

### 11.1 Common rules

- A secondary frame has a title bar with text and a Close button, no Minimize or Maximize, and no task-bar entry. It stays above its
  owner, hides and returns with it, and closes with it.
- Its title says what it is for: the command's name without the ellipsis and without the menu's name (`Print`, not `File Print...`).
- **Prefer modeless.** Make a dialog modal only when the command cannot go on without the answer. Block the whole system only when
  data is about to be lost.
- Open it where it last was; otherwise centred on its owner, fully on the screen.
- Fill it from the context: the selection's current values, the last values used.
- It is normally not resizable. Extra options can unfold behind a `More >>` button.
- A dialog may open one further dialog on top of itself, offset down and right. Go no deeper.
- `Tab` order follows reading order, starting at the main field and ending at the transaction buttons. Static labels are not tab stops.
  A radio group is one stop.

### 11.2 Default button

- `Enter` presses the default button, which has a heavier outline. It is the most likely safe action, usually OK.
- **A destructive action is never the default** (not `Delete`, not `Replace All`).
- When focus is on another command button, that button becomes the default until focus leaves it.
- When focus is in a control that needs `Enter` (a multi-line text box), there is no default.
- `Esc` is always Cancel.

### 11.3 Button placement

- Transaction buttons are in a row along the bottom, right-aligned, or stacked at the top right. Pick one per application.
- Order: the default button first, OK and Cancel together, Help last. With no OK, Cancel follows the action buttons.
- Buttons are the same width. A button that serves one field sits next to that field and is not the default.
- A button inside a tab page acts on that page only. A button outside the pages acts on the whole frame.

### 11.4 Validation

Validate as close to the entry as possible: constrain with the right control and sensible defaults, reject bad characters as typed, or
check when focus leaves the field. If the check has to wait for OK, keep the dialog open, say what is wrong, and put the focus on the
field at fault.

### 11.5 Property sheets

- A property sheet shows the properties of an object and is opened by **Properties** (`Alt+Enter`). It is modeless.
- Title: `<Object name> Properties`. For several objects of one type use the type name; for a mixed selection, `Selection Properties`.
- Group related properties on tab pages.
- Buttons, outside the pages: **OK** applies everything and closes. **Apply** applies and stays open. **Cancel** discards what has not
  been applied and closes.
- The Close button is not Cancel: with unapplied changes, ask whether to apply.
- Show a sample of the effect where it helps (a font, a colour).
- A property inspector is a palette that always shows the properties of the current selection and applies changes at once.

### 11.6 Standard dialogs

Use the system's file, print, colour and font dialogs; do not write a private version.

- **Open** and **Save As** remember the folder for the session, list file types by their friendly names, and hide extensions unless the
  user shows them. Save As offers the current name, or the type name for a new document. A full stop inside a name is not treated as an
  extension the user meant.
- A file type is not the same as a format option. Colour depth or compression gets its own control.
- **Print** offers what to print, the number of copies, the printer, and a button to the printer's properties.
- **Find** and **Replace** are modeless and stay open for repeated use.

### 11.7 Palettes

A palette is a small modeless frame of tools or settings, with a short title bar and only a Close button. A toolbar dragged off its
edge becomes one, and a palette dragged to an edge docks again. Remember its position and size. "Always on top" is the user's choice
and keeps it above its own application only.

### 11.8 Messages

Design the situation away first: check for space before starting, disable the command that cannot work. A message is the last resort.

- **Three kinds, each with its icon:**

| Kind | For | Buttons |
| --- | --- | --- |
| Information | The result of a command; nothing to decide. | OK |
| Warning | A decision is needed before going on, typically something that cannot be undone. | Yes / No, or named actions, plus Cancel |
| Error | Something failed and must be corrected before the work can continue. | OK, or the corrective actions |

- The title is the name of the document or application it is about. Never "Warning" or "Error".
- The text says **what happened, why, and what the user can do**, in two or three lines:
  `"Report" could not be saved because the disk is full. Save it on another disk, or free some space and try again.`
  Not: `Insufficient disk space.`
- Use the user's words. No internal names, no codes in the sentence (a reference number may go at the end), no blame, and avoid the
  word "error".
- Prefer buttons that name the action (`Save`, `Don't Save`, `Cancel`) to Yes and No whenever the question could be misread.
- The default button is the safest choice.
- **Cancel** means everything is put back as it was. If it cannot be, the button is **Stop**.
- One message per event. Never a chain of them.
- A message from a frame that is not active must not appear on top of the user's work: flash the frame's task-bar entry a few times,
  leave it highlighted, and show the message when the user comes to it. The only exception is imminent loss of data.
- Never show critical information in something that disappears by itself. A notification that times out must also be recorded
  somewhere the user can find it later.

## 12. Layout

- Lay out in reading order: left to right and top to bottom, the most important thing at the upper left. Mirror for right-to-left
  languages.
- Measure in **layout units** derived from the interface font, so a layout follows the font and the display scale: a horizontal unit is
  a quarter of the font's average character width and a vertical unit an eighth of its line height.

| Item | Size (units) |
| --- | --- |
| Single-line control height (button, text box, drop-down) | 14 |
| Space between controls | at least 3 |
| Space between groups | more than between the controls of one group |
| Margin from the frame edge | 7, the same on every side |
| Largest dialog or property sheet | 234 wide by 263 high |

- A dialog must fit the smallest supported screen with room to spare; split it into pages before it grows past the largest size above.
- Align the left edges of labels and of fields. A label is above its field or to its left, on the same text baseline, and ends with a
  colon.
- Put related controls close together (the button that acts on a list is beside the list). Separate groups with space first and with a
  group box second.
- Give text room to grow in translation: 30% for ordinary labels, 100% for short ones. Never depend on a string's width.
- When the frame resizes, the content area grows; buttons and labels keep their size and their edges.

## 13. Visual design

The look is a classic bevelled gray desktop. All colours come from the theme; **no control hard-codes a colour.**

### 13.1 Colour

- Colour is never the only carrier of a meaning. Pair it with shape, position or text. Design in gray first, then add colour.
- Use few colours, muted. Saturated colour is for small things that must be noticed.
- Use theme colours in their pairs: text on face, field text on field, selection text on selection, title text on title. Never combine
  the foreground of one pair with the background of another, or a different theme will make it unreadable.

### 13.2 Three-dimensional look

- The light comes from the upper left: lit edges are at the top and left, shadows at the bottom and right.
- Depth means "you can interact with this". Do not bevel things that do nothing.
- Use the standard borders, each built from one-pixel lines:

| Border | Used for |
| --- | --- |
| Raised (outer and inner) | Frames, menus, command buttons at rest, scroll arrows |
| Sunken (outer and inner) | Editable fields, lists, check boxes, work areas; a pressed button |
| Sunken, outer line only | Status-bar fields |
| Etched (sunken outer, raised inner) | Group boxes, menu separators |

- An editable field has the field colour inside; a read-only one has the face colour.

### 13.3 Fonts

- Use the system interface font everywhere, at the size the user set. Title bars use its bold weight.
- Bold elsewhere only to mark the default (the default menu item, the default button's outline). Avoid italics and decorative faces at
  interface sizes.

### 13.4 Icons and pictures

- Supply every icon at 16, 32 and 48 pixels, drawn for each size, not scaled. The small one keeps the shape and the telling detail of
  the large one.
- Every application has an icon and so does every file type it owns. A document icon is different from the application icon and shares
  an element with it.
- Draw a recognisable real-world object, a noun and not an action, with only the detail needed to recognise it. One style across a set:
  the same light from the upper left, the same perspective, no heavy black outline.
- No words or letters in pictures. Avoid images that mean something only in one culture.
- Smooth edges inside an icon, never on its outer edge, so that it sits on any background.
- A picture is always accompanied by the object's name as text.

### 13.5 Showing state

- **Selected:** the system selection colours or handles ([section 5](#5-selection)).
- **Being dragged:** a translucent or outline image following the pointer.
- **Open elsewhere:** content that is open for editing in another frame is shown with diagonal hatching over it.
- **Link:** a small arrow badge at the lower left of the icon.
- **Animation** explains or gives feedback; it is never decoration and never makes the user wait. It can be interrupted and can be
  turned off. Anything that blinks follows the system cursor blink rate.

## 14. Text and wording

### 14.1 Capitalization

| Style | Use for | Examples |
| --- | --- | --- |
| Title: every word capitalized except articles, conjunctions and short prepositions that are not first or last | Menu titles and items, command buttons, frame and dialog titles, toolbar tooltips, icon names | `Save As`, `Go To`, `Select All`, `Always on Top` |
| Sentence: only the first word and proper names | Labels of check boxes, radio buttons, text boxes, lists, group boxes and tabs; messages; status text | `Find whole words only`, `Working folder:`, `Print to file` |

Names the user typed are shown exactly as typed.

### 14.2 Wording

- Use the user's vocabulary, never the implementation's. "Picture", not the name of the picture format.
- Short, common words. No jargon, no abbreviations the user did not choose, no humour that will not translate.
- Never build a sentence by gluing strings together; word order differs between languages. Use one whole string with placeholders.
- The same thing is called the same thing everywhere.

### 14.3 Names of files and types

- Show file names in full, as typed. Show the extension only if the user asked or the type is unknown.
- Each file type has a friendly name of at most 40 characters (`Spreadsheet`, `Text Document`) and a short form of at most 15 for menus
  and lists.
- A path shown where space is short is abbreviated in the middle, keeping the first and last parts: `/Desktop/.../Reports/May`.
- An extension says what type a file is and nothing else. A backup is `Backup of Report`, not the report with another extension.

## 15. User assistance

Help is on demand, brief, and does not make the user lose their place.

| Form | Answers | Rules |
| --- | --- | --- |
| Tooltip | "What is this called?" | A name, a few words, title capitalization. |
| Status-bar text | "What does this command do?" | One sentence starting with a present-tense verb: `Copies the selection to the clipboard.` For a disabled command, say why: `Not available because nothing is selected.` |
| Context help (`F1`, `Shift+F1`, a `?` button on a dialog's title bar, a menu item) | "What is this and why would I use it?" | A small pop-up next to the item. Starts with a verb: `Sets how fast the pointer moves.` Plain words: "Reverses the last action", not "Undoes". No how-to. |
| Task help | "How do I...?" | Numbered steps, one method only (the simplest), four steps or fewer if possible, no background. Refer to controls by their labels. May offer a button that performs or opens the step. |
| Reference help | Everything else | Each topic has a title that says where the user is. Contents, an index of the words users actually use (beginner words and synonyms included), and search. |

- A **Help** button in a dialog explains that dialog as a whole. In a message it explains causes and remedies at more length.
- **Wizards** lead the user through an infrequent or complicated task in one frame with several pages. They are an addition to the
  direct way of doing the task, not a replacement for it.
  - Buttons along the bottom: `< Back`, `Next >`, `Finish`, `Cancel`. Back is disabled on the first page. Finish is enabled as soon as
    the defaults would give a good result.
  - Every control has a default, so that `Enter` alone gets through.
  - Few choices per page, more pages before a crowded page. Avoid controls that open further dialogs.
  - The first page says what the wizard will do. The last page says it is ready and what happens after Finish.
  - Write conversationally, to "you": `Which layout do you want?`, not `Choose a layout.` As few words as possible.

## 16. Fitting into the system

- **Files and settings.** An application keeps its settings, caches and state under its own folder in the application store, and
  nowhere else. The user's documents default to the matching folder under the desktop. Use the path constants; see
  [FileSystemLayout.md](FileSystemLayout.md).
- **State.** Save the frame's state when it closes and restore it when it opens. Write settings so that a missing or damaged one falls
  back to a default.
- **File types.** Register each type the application owns with an icon, a friendly name, a default command (what double-click does) and
  any others (Print). Never take over a type another application registered without the user's say. A printable type can be printed
  from its icon without opening the application's frame, with feedback that printing started.
- **Creating.** Register document types with the container's **New** command so the user can create one without starting the
  application first.
- **Launcher and task bar.** One entry per application in the launcher menu, not one per helper. A task-bar entry per open primary
  frame, supplied by the system.
- **Status area.** Put an indicator there only for something global the user needs to watch while working elsewhere. A click shows a
  small pop-up of controls, a secondary click a menu, a double-click the menu's default; a tooltip says what it is.
- **Trash.** Files the application deletes go to the trash through the system, never straight off the disk.
- **Installing and removing.** Everything installed is recorded, and uninstalling removes all of it, files and settings. An installer
  has a default for every question, states the space it needs, shows progress, and can be cancelled cleanly. Shared components are
  counted and removed only when nothing uses them.
- **System settings.** Read colours, fonts, sizes and the display scale from the system and follow them when they change while running.
- **Hardware and environment changes.** Cope with a change of screen size or scale (keep frames on screen), removal of a disk (save and
  close what was on it), loss of the network, and low power (offer to save).
- **Stay responsive.** Long work runs in the background. The frame keeps painting, can be moved and minimized, and offers Cancel.
- **Embedded content.** Content of another type placed inside a document behaves as one object of the document: it is selected, moved,
  sized and deleted like any other, and shows handles when selected. Its own commands come from its type; its default command edits it,
  in place if possible. While it is edited in place the frame's title and the File menu remain the document's, only one level is active
  at a time, the rest of the document does not shift, and `Esc` or a click outside ends the editing. Changes become part of the
  document at once, with no "update?" question, and share the frame's single undo history. Linked content is shown the same way with a
  link badge; editing it opens its source, and it updates automatically unless the user chose manual updating.

## 17. Accessibility, sound and other languages

### 17.1 Accessibility

- Follow the standard conventions and use the standard controls. Assistive tools rely on them.
- **Keyboard:** every function is reachable, and the focus is always visible.
- **Never colour alone, never sound alone, never timing alone.**
- **Names:** every control, icon and frame has a text name even when it is not shown. A label comes directly before its control and
  ends with a colon. A frame always has a title, unique among the application's frames.
- **No action on navigation:** moving the focus through a dialog never toggles, opens or runs anything.
- **No time limits** on reading or responding. Where one is unavoidable the user can change it. Nothing important is shown only briefly
  or only while the pointer hovers.
- **Scale:** the interface works with larger fonts and at larger display scales. Text is drawn as text, not as a picture of text.
- **Contrast:** honour the high-contrast setting. Use its colours and remove pictures and patterns from behind text.
- **Flashing:** nothing flashes faster than the cursor blink rate, and that rate is the user's setting.
- **Basic pointer use:** single clicks. Double-clicks, drags and modified clicks are shortcuts.
- Test: use it for a day without a pointer; in a high-contrast theme; with the largest font setting. Whatever became impossible or
  invisible is a defect.

### 17.2 Sound

- Sound is an addition. Everything it tells the user is also visible (a flash of the task-bar entry, a message, a caption).
- Follow the system volume. Sounds for events are the user's to assign or turn off.
- When the user has asked for sounds to be shown, give speech and meaningful sounds a caption.

### 17.3 Other languages and regions

- All interface text lives in string tables, not in code.
- Translated text is longer and its words come in a different order. See the room to leave in [section 12](#12-layout).
- One string, one use. The same word in two places may need two translations.
- Dates, times, numbers, currency, units, sort order, paper sizes and the first day of the week come from the regional settings. An
  application may let the user override them for itself, never for the system.
- Shortcut letters that are mnemonics in one language may need changing in another. Do not use punctuation keys as shortcuts.
- Do not assume a character set or that one character is one byte.

## 18. Review checklist

Run through this before calling an interface change finished.

**Behaviour**

- [ ] Every function works with the keyboard alone, and the focus is always visible.
- [ ] Every basic function works with single primary clicks.
- [ ] Standard shortcuts and standard commands keep their standard meanings.
- [ ] Every selectable thing has a context menu (secondary click, `Shift+F10`, Menu key) ending in Properties where it has properties.
- [ ] Nothing exists only on a toolbar, only in a context menu, only in the status bar, or only as a shortcut.
- [ ] Destructive actions can be undone or are confirmed, and are never the default button.
- [ ] Closing with unsaved changes asks save / discard / cancel.
- [ ] Opening something that is already open activates the existing frame.
- [ ] Nothing takes focus from what the user is doing.
- [ ] Long operations show progress, can be cancelled, and leave the frame responsive.
- [ ] Drag and drop has a command equivalent, shows feedback, and cancels with `Esc`.

**Frames and dialogs**

- [ ] The title names the object viewed, as the file explorer shows it.
- [ ] Size, position and view state are saved and restored, and adjusted to the current screen.
- [ ] Dialogs are modeless unless they must be modal.
- [ ] `Enter` presses a safe default; `Esc` cancels; the `Tab` order follows reading order.
- [ ] OK, Cancel, Apply, Close and Stop mean what [6.3](#63-committing-and-undoing) says.
- [ ] Commands that need more input end in `...`, and no others do.
- [ ] Commands that do not apply are disabled, not left to fail.

**Messages**

- [ ] The situation could not have been prevented by the design.
- [ ] It says what happened, why, and what to do, in the user's words, in two or three lines.
- [ ] The right kind (information, warning, error), a title naming the object, action-named buttons, a safe default.

**Appearance**

- [ ] All colours come from the theme and are used in their pairs.
- [ ] All sizes follow the font and the display scale; nothing assumes a screen size.
- [ ] Standard borders, states and light direction; depth only on interactive things.
- [ ] Title capitalization for commands and titles, sentence capitalization for labels and messages.
- [ ] Every control has a label or a tooltip, an access key where it has a label, and all the states in [9.1](#91-states-every-control-has).
- [ ] No meaning is carried by colour alone or by sound alone.
- [ ] Labels have room to grow; no sentence is assembled from fragments.

## 19. How this maps to the toolkit

The toolkit is in `src/Gui` and the built-in applications in `src/Desktop`. Before adding a control or a dialog, look for an existing one.

| Guideline concept | Toolkit |
| --- | --- |
| Theme colours and their pairs | `gui::Theme` (`Theme.hpp`): `face`/`text`, `field`/`text`, `selection`/`selection_text`, `title`/`title_text`, `inactive_title`/`inactive_title_text`, `disabled_text`, and the bevel colours `highlight`, `light`, `shadow`, `dark` |
| Frame, title bar, frame buttons, size borders | `gui::Frame` |
| Several documents as tabs in one frame | `gui::TabFrame` |
| Frame order, activation, modal dialogs, `Alt+Tab` | `gui::WindowManager`, `gui::WindowSwitcher` |
| Warning with a decision | `gui::MessageDialog`: the default button comes first, `Enter` chooses it, `Esc` and the Close button decline |
| Information, warning or error with one OK button | `gui::NoticeDialog` with `NoticeKind` |
| Notification that does not interrupt | `ToastCenter` |
| Menu bar and drop-down menus, context menus, menu buttons | `Menu`, `ContextMenu` (see `Control::prepare_context_menu`), `MenuButton` |
| Toolbar, tooltip, status and progress | `ToolStrip`, `Tooltip`, `Label`, `ProgressBar` |
| Choice controls | `Button`, `CheckBox`, `Toggle`, `RadioButton`, `DropDown`, `ListView`, `TreeView`, `DataGrid`, `SpinBox`, `Slider`, `TabControl`, `DatePicker` |
| Text | `TextBox` (single field), `TextEditor` (multi-line) |
| Scrolling | `ScrollPanel`, `ScrollBar` |
| Pointer shapes | `gui::CursorShape` via `Control::cursor_shape()` |
| Drag and drop | `Control::accepts_drop`, `on_drag_over`, `on_drop`, `on_drag_ended` |
| Layout that follows the frame size | `gui::DockStyle` and `Control::layout()` |
| File, print, colour dialogs | `FileDialog`, `PrintDialog`, `ColorDialog` in `src/Desktop` |
| Property sheets | `FileProperties`, `DeviceProperties` and the other `*Properties` frames in `src/Desktop` |

Toolkit rules that follow from the guidelines:

- Event handlers run on the frame's handler thread so that painting and input never wait for them. Keep it that way: do long work
  there or on a worker, never on the input path, and report progress.
- Paint only with theme colours through `Painter`. A new control implements focus, pressed, unavailable and keyboard behaviour before
  it is used anywhere.
- Sizes scale with the display (see [Display.md](Display.md)). Do not add pixel constants that ignore the scale.
- Paths come from the layout helpers (see [FileSystemLayout.md](FileSystemLayout.md)).
