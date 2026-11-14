/*
 * TheoryCraftOS - a minimal UEFI x86_64 operating system.
 * Copyright (C) 2026 Christopher Schaefer
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published
 * by the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

// The toolkit's share of the guidelines: access keys (4.1), the menu keys (8.1), context menus without shortcut texts (8.2), the default
// button and the three-way question (11.2, 11.8), message titles (11.8) and undo in the text editor (6.1). (included by tests/UiGuideTest.cpp)

static void ui_guide_toolkit() {
  TestAllocator a;
  // --- The rule that hands out the letters: the first letter, else a free consonant, else a free vowel; none when all are taken.
  {
    AccessKeys keys;
    CHECK(keys.pick("Save") == 0 && keys.pick("Save As...") == 2 && keys.pick("Send") == 2 && keys.pick("See") == 1);
    CHECK(keys.pick("S") == AccessKeys::kNone && keys.pick("123") == AccessKeys::kNone);
    CHECK(keys.claim("Exit", 'x') == 1 && keys.claim("Extra", 'x') == AccessKeys::kNone && keys.claim("Exit", 'q') == AccessKeys::kNone);
    CHECK(AccessKeys::key_of("Exit", 1) == 'x' && AccessKeys::key_of("Exit", 9) == 0 && AccessKeys::key_of("Exit", -1) == 0);
  }
  // --- A message is one sentence with placeholders, never pieces glued together.
  {
    String text(a);
    CHECK(desktop::format_message(&text, "\"%1\" could not be saved because %2. %3 100%%.", "Report", "the disk is full", "Free some space:") &&
          text.view().equals("\"Report\" could not be saved because the disk is full. Free some space: 100%."));
    CHECK(desktop::format_message(&text, "%2 before %1, and a lone % or %9 stays", "one", "two") && text.view().equals("two before one, and a lone % or %9 stays"));
    CHECK(desktop::format_message(&text, "") && text.empty());
  }
  {
    Screen screen(a, 640, 480);
    Theme theme;
    Window w(a, screen.renderer, theme, {640, 480});
    WindowManager mgr(a);
    mgr.set_dock(DockStyle::Fill);
    w.add(&mgr);

    struct Counter {
      int count = 0;
      static void fn(void* context, Control&) { ++static_cast<Counter*>(context)->count; }
      Handler handler() { return {&Counter::fn, this}; }
    };

    Frame frame(a);
    frame.set_title("Letters");
    frame.set_bounds({20, 20, 420, 320});
    MenuBar bar(a);
    bar.set_dock(DockStyle::Top);
    Counter opened, saved, saved_as, exited;
    Menu* file = bar.add_menu("File");
    file->add_item("Open...", opened.handler(), "Ctrl+O");
    file->add_item("Save", saved.handler(), "Ctrl+S");
    file->add_item("Save As...", saved_as.handler());
    file->add_separator();
    MenuItem* exit_item = file->add_item("Exit", exited.handler());
    exit_item->access_key = 'x';
    Menu* format = bar.add_menu("Format");
    format->add_item("Font...");
    Label name_label(a);
    name_label.set_text("File name:");
    name_label.set_dock(DockStyle::Top);
    Label status(a);            // no colon: it names nothing, so it has no access key
    status.set_text("Ready");
    status.set_dock(DockStyle::Bottom);
    TextBox name(a);
    name.set_dock(DockStyle::Top);
    CheckBox whole(a);
    whole.set_text("Find whole words only");
    whole.set_dock(DockStyle::Top);
    Button apply(a), ok(a), cancel(a), find(a);
    Counter applied, found;
    apply.set_text("Apply");
    apply.on_click(Handler::immediate(&Counter::fn, &applied));
    find.set_text("Find Next");
    find.on_click(Handler::immediate(&Counter::fn, &found));
    ok.set_text("OK");
    cancel.set_text("Cancel");
    for (Button* b : {&apply, &find, &ok, &cancel}) b->set_dock(DockStyle::Top);
    frame.add(&bar);
    frame.add(&name_label);
    frame.add(&name);
    frame.add(&whole);
    frame.add(&apply);
    frame.add(&find);
    frame.add(&ok);
    frame.add(&cancel);
    frame.add(&status);
    CHECK(mgr.add_window(&frame));
    w.layout();
    w.render();     // (painting hands out the letters)

    // Unique in the frame, in the order the labels appear: File, fOrmat (F is taken), file Name (F taken, then the first free consonant),
    // find Whole words (F, N taken), Apply, finD next.
    auto key_of = [](Control& c) { return AccessKeys::key_of(c.access_label(0), c.access_index(0)); };
    CHECK(AccessKeys::key_of(bar.access_label(0), bar.access_index(0)) == 'f' && AccessKeys::key_of(bar.access_label(1), bar.access_index(1)) == 'r');
    CHECK(key_of(name_label) == 'l' && key_of(whole) == 'n' && key_of(apply) == 'a' && key_of(find) == 'd');
    CHECK(ok.access_label_count() == 0 && cancel.access_label_count() == 0 && status.access_label_count() == 0);   // Enter and Escape are theirs
    CHECK(menus_have_access_keys(frame, bar) && standard_menu_order(bar));

    // Alt+letter: the button is pressed, the check box toggled, the label hands the focus to the field after it.
    w.set_focus(&apply);
    alt(w, 'd');
    CHECK(found.count == 1 && w.focus() == &find);
    alt(w, 'n');
    CHECK(whole.checked() && w.focus() == &whole);
    alt(w, 'l');
    CHECK(w.focus() == &name);
    alt(w, 'a');
    CHECK(applied.count == 1);
    // ... also while a text field has the focus, and a letter no label has is not typed into it.
    w.set_focus(&name);
    alt(w, 'n');
    CHECK(!whole.checked());
    w.set_focus(&name);
    alt(w, 'q');
    CHECK(name.text().empty());
    // A disabled control does not answer.
    apply.set_enabled(false);
    alt(w, 'a');
    CHECK(applied.count == 1);
    apply.set_enabled(true);
    // An asked-for letter is served before the automatic ones.
    find.set_access_key('n');
    frame.assign_access_keys();
    CHECK(key_of(find) == 'n' && key_of(whole) != 'n');
    find.set_access_key(0);

    // Alt+F opens the File menu with its first item highlighted; a letter then runs the item with that access key.
    w.set_focus(&name);
    alt(w, 'f');
    CHECK(bar.open_menu_index() == 0 && w.popup());
    CHECK(AccessKeys::key_of("Open...", file->item(0)->access_index) == 'o' && AccessKeys::key_of("Save", file->item(1)->access_index) == 's' &&
          AccessKeys::key_of("Save As...", file->item(2)->access_index) == 'v' && AccessKeys::key_of("Exit", exit_item->access_index) == 'x');
    press(w, key::kA + ('v' - 'a'));
    CHECK(saved_as.count == 1 && saved.count == 0 && bar.open_menu_index() < 0 && !w.popup());
    alt(w, 'f');
    press(w, key::kA + ('x' - 'a'));
    CHECK(exited.count == 1);
    // A disabled item does not run from its letter, and the menu stays open.
    file->item(1)->set_enabled(false);
    alt(w, 'f');
    press(w, key::kA + ('s' - 'a'));
    CHECK(saved.count == 0 && bar.open_menu_index() == 0);
    press(w, key::kEscape);
    file->item(1)->set_enabled(true);
    // F10 goes into the menu bar; Left and Right wrap around the titles, Up and Down around the items; Enter chooses; Escape leaves.
    press(w, key::kF1 + 9);
    CHECK(bar.open_menu_index() == 0);
    press(w, key::kLeft);
    CHECK(bar.open_menu_index() == 1);
    press(w, key::kRight);
    CHECK(bar.open_menu_index() == 0);
    press(w, key::kUp);       // from the first item up: the last one
    press(w, key::kEnter);
    CHECK(exited.count == 2 && bar.open_menu_index() < 0);
    press(w, key::kF1 + 9);
    press(w, key::kEscape);
    CHECK(bar.open_menu_index() < 0 && !w.popup());

    // A context menu leaves out the shortcut texts (they belong to the menu bar): it is as wide as a menu without them.
    ContextMenu with(a), without(a);
    with.add_item("Cut", {}, "Ctrl+Shift+Alt+X");
    without.add_item("Cut");
    CHECK(with.show(w, 10, 10));
    const uint32_t with_width = w.popup()->bounds().width;
    with.close_menu();
    CHECK(without.show(w, 10, 10) && w.popup()->bounds().width == with_width);
    without.close_menu();

    mgr.close_window(&frame);
  }

  // --- A question with three ways out: Save (the default, with the heavier outline), Don't Save, Cancel.
  {
    Screen screen(a, 640, 480);
    Theme theme;
    Window w(a, screen.renderer, theme, {640, 480});
    WindowManager mgr(a);
    mgr.set_dock(DockStyle::Fill);
    w.add(&mgr);
    MessageDialog ask(a);
    ask.set_save_changes("Notes");
    CHECK(ask.title().equals("Notes") && ask.accept_button().text().equals("Save") && ask.alternate_button().text().equals("Don't Save") &&
          ask.decline_button().text().equals("Cancel") && ask.alternate_button().visible());
    CHECK(ask.accept_button().is_default() && !ask.alternate_button().is_default() && !ask.decline_button().is_default());
    CHECK(ask.show(mgr) && w.focus() == &ask.accept_button());
    w.layout();
    // left to right: Save, Don't Save, Cancel
    CHECK(ask.accept_button().window_origin().x < ask.alternate_button().window_origin().x &&
          ask.alternate_button().window_origin().x < ask.decline_button().window_origin().x);
    press(w, key::kEnter);
    CHECK(!ask.showing() && ask.choice() == MessageDialog::Choice::Accept && ask.accepted());
    CHECK(ask.show(mgr));
    w.layout();
    alt(w, 'd');      // Don't Save has an access key; Cancel has none (Escape is its key)
    CHECK(!ask.showing() && ask.choice() == MessageDialog::Choice::Alternate && !ask.accepted());
    CHECK(ask.show(mgr));
    press(w, key::kEscape);
    CHECK(!ask.showing() && ask.choice() == MessageDialog::Choice::Decline);
    // set_texts() makes it a two-button question again.
    ask.set_texts("Trash", "Empty the trash?", "", "Empty", "Cancel");
    CHECK(!ask.alternate_button().visible());

    // --- A message is titled with what it is about: the name given, else the window the user was in. Never "Error".
    Frame doc(a);
    doc.set_title("Report - Text Editor");
    doc.set_bounds({10, 10, 300, 200});
    CHECK(mgr.add_window(&doc));
    NoticeDialog notice(a);
    CHECK(notice.show(mgr, NoticeKind::Error, "The file could not be saved because the disk is full.") && notice.title().equals("Report - Text Editor"));
    CHECK(notice.ok_button().is_default());
    CHECK(notice.show(mgr, NoticeKind::Warning, "Again.") && notice.title().equals("Report - Text Editor"));   // replacing the text keeps the title
    notice.close();
    CHECK(notice.show(mgr, NoticeKind::Information, "Sent to the printer.", "Printer") && notice.title().equals("Printer"));
    notice.close();
    mgr.close_window(&doc);
    CHECK(notice.show(mgr, NoticeKind::Information, "Nothing is open.") && notice.title().equals("TheoryCraftOS"));
    notice.close();
  }

  // --- Undo in the text editor: a run of typing is one change, so is a run of erasing; Ctrl+Z, Ctrl+Y, and the context menu.
  {
    Screen screen(a, 400, 300);
    Theme theme;
    Window w(a, screen.renderer, theme, {400, 300});
    TextEditor e(a);
    e.set_bounds({0, 0, 400, 300});
    w.add(&e);
    w.layout();
    w.set_focus(&e);
    CHECK(!e.can_undo() && !e.can_redo() && !e.undo() && !e.redo());
    type_text(w, "hello");
    CHECK(e.can_undo() && e.modified());
    press(w, key::kSpace);
    type_text(w, "world");
    press(w, key::kBackspace);
    press(w, key::kBackspace);
    CHECK(e.text().equals("hello wor"));
    ctrl(w, 'z');                                // the erasing
    CHECK(e.text().equals("hello world") && e.can_redo());
    ctrl(w, 'z');                                // the typing, all of it
    CHECK(e.text().empty() && !e.can_undo() && !e.modified());
    ctrl(w, 'y');
    CHECK(e.text().equals("hello world") && e.caret() == 11);
    ctrl(w, 'z', modifier::kLeftShift);          // Ctrl+Shift+Z is redo too
    CHECK(e.text().equals("hello wor") && !e.can_redo());
    // Moving the caret ends a run; a new change forgets what could be redone.
    e.set_caret(0);
    type_text(w, ">");
    ctrl(w, 'z');
    CHECK(e.text().equals("hello wor") && e.can_redo());
    type_text(w, "!");
    CHECK(!e.can_redo());
    // Selecting and replacing, pasting and cutting are changes of their own.
    e.select_all();
    type_text(w, "x");
    CHECK(e.text().equals("x"));
    ctrl(w, 'z');
    CHECK(e.text().equals("!hello wor"));
    // Saved text is unmodified; undoing past the save point is a change again, and redoing back to it is not.
    e.mark_saved();
    CHECK(!e.modified());
    type_text(w, "?");
    CHECK(e.modified());
    ctrl(w, 'z');
    CHECK(!e.modified());
    ctrl(w, 'z');
    CHECK(e.modified());
    // The context menu starts with Undo, enabled only when there is something to take back.
    e.set_text("fresh");
    CHECK(!e.can_undo() && !e.can_redo() && !e.modified());
    right_click(w, 50, 50);
    CHECK(w.popup() && e.context_menu() && e.context_menu()->menu().item(0)->text.view().equals("Undo") && !e.context_menu()->menu().item(0)->enabled());
    w.close_popup();
    type_text(w, "a");
    right_click(w, 50, 50);
    CHECK(e.context_menu()->menu().item(0)->enabled());
    CHECK(click_popup_item(w, &e.context_menu()->menu(), "Undo") && e.text().equals("fresh"));
    // Nothing is taken back in a read-only editor.
    type_text(w, "b");
    e.set_read_only(true);
    CHECK(!e.undo());
    e.set_read_only(false);
    // The history is bounded: far more changes than it holds leave it at its limit, and the newest are the ones kept.
    e.set_text("");
    for (int i = 0; i < 200; ++i) {
      type_text(w, "a");
      e.set_caret(e.caret());      // (the caret does not move, so...)
      press(w, key::kEnter);       // ...a line break, which is a change of its own, separates the runs
    }
    uint32_t steps = 0;
    while (e.undo()) ++steps;
    CHECK(steps == TextEditor::kMaxUndoSteps && !e.text().empty());
  }
  CHECK(a.live == 0);
}
