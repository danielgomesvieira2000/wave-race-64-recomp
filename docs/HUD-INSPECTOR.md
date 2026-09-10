# The HUD inspector

A window inside RT64's developer UI that lists every 2D element the port
classified this frame, outlines one on the screen when you hover it, and lets you
change its widescreen class from a dropdown while the game runs.

Two halves: [**using it**](#using-it) and [**putting it in another
project**](#putting-it-in-another-project). The second half is written so that a
different N64 port on RT64 can have the same tool in an afternoon.

Why it exists is in [PORTING.md](PORTING.md) §7, *The HUD inspector*. The short
version: the previous instrument was a trace read afterwards and matched to a
screenshot by eye, a menu wipe lasts six tenths of a second, and an element's
identity is not something a picture can show.

---

## Using it

### Turning it on

**Press F1.** In any build, released or not, with nothing set beforehand. F1
again closes it.

F1 is RT64's own shortcut for its developer UI, and the port's window is drawn
inside that UI, so both appear together: RT64's **Game editor** on one side and
**Wave Race HUD** on the other. That pairing is the point -- half of what you
need is already in RT64's half (see
[What RT64 already does](#what-rt64-already-does)).

A debug menu that only exists in a build made for it is a debug menu nobody has
when they need it: the person looking at a misplaced menu element is running the
game they downloaded. Nothing is drawn until F1 is pressed -- RT64 creates its
inspector on the keystroke, and `State::inspect()` returns immediately while
there is none -- so leaving it available costs a null check per frame.

The element list needs a build with the frontend on (`build-fe`), because
**`build-rt` has no display-list rewriter at all** and so has nothing to list.
RT64's own half of the menu works in both.

`WR64_INSPECTOR=0` turns the port's half off, for an A/B against the rewriter's
own classification. RT64's half stays on F1 regardless.

### The other keys

Developer mode arms three more of RT64's shortcuts for everyone, so they are part
of the shipped build too:

| Key | Does |
|---|---|
| **F1** | Opens and closes this menu |
| F2 | **Unbound.** RT64 uses it to toggle ray tracing for the session, with no menu entry saying so and nothing on screen to explain what changed. `tools/patch_rt64_inspector.py` removes the case from both key filters, so the key passes through to the game like any other. |
| F3 | Views RDRAM. Visibly reversible, plainly diagnostic. |
| F4 | Toggles texture replacements. Same. |

### The window

```
state 0x28   frame 1851   5 elements
[ ] Hold this frame   [ Save to hud.json ]   [ Clear overrides ]
[ part of an identity ] filter
 #  identity          x         y         proj        class
 0  tex:0x0100FAB0    114..184  0..81     persp rect  center   v
 1  tex:0x01005748    9..310    21..218   persp rect  stretch  v
 2  tex:0x010331D0    241..273  21..33    persp rect  center   v
 3  tex:0x0103D8D8    257..298  34..54    persp rect  right    v
 4  tex:0x010515A8    22..298   197..217  persp rect  stretch  v
```

| Column | Meaning |
|---|---|
| `#` | Position in this frame's display list. Not stable between frames. |
| `identity` | What `hud.json` and the traces call this element: `tex:0x…` is the texture image address, `dl:0x…` the display list it was drawn from. Both are shown when the element has both. **This is the string you tag.** |
| `x`, `y` | Extent in the game's own 320x240 pixels, before any widescreen arithmetic. |
| `proj` | `persp`/`ortho` -- which matrix was loaded; `rect`/`tris` -- a rectangle command or a run of triangles. |
| `class` | What the rewriter decided, as a dropdown. A `*` after it means you have overridden it. |

The header line's `state` is the game's own state variable (0x03 title, 0x28
racing, 0x0B the rider-select wipe -- see [GAME-INTERNALS.md](GAME-INTERNALS.md),
the game-state machine).

| Control | What it does |
|---|---|
| **Hover a row** | Outlines that element on the screen in yellow. |
| **Click a row** | Keeps the outline up in blue, so it stays while you use the dropdown. Click again to deselect. |
| **class dropdown** | `as classified` drops the override; `center` / `left` / `right` / `stretch` apply from the **next frame**. No rebuild, no restart. |
| **Hold this frame** | Freezes the *list* on the frame that was current when you ticked it, while the game keeps running. For anything animated. |
| **Save to hud.json** | Writes every override into the tag table in the settings folder. Merges -- entries you did not touch survive, and an identity is removed from the other three lists first. |
| **Clear overrides** | Drops every override. Does not touch `hud.json`. |
| **filter** | Substring match on either identity. |

### The four classes

| Class | What the rewriter emits | Use it for |
|---|---|---|
| `center` | Nothing. The element stays in the 4:3 box in the middle. | Anything that should keep its shape and position: HUD readouts, menu text. |
| `left` | Extended origin `G_EX_ORIGIN_LEFT` | An element pinned to the left edge of the screen. |
| `right` | Extended origin `G_EX_ORIGIN_RIGHT` | An element pinned to the right edge. |
| `stretch` | `gEXSetRectAspect(G_EX_ASPECT_STRETCH)`, with the element's own origins left `G_EX_ORIGIN_NONE` | Backgrounds, full-screen overlays, wipes -- anything that should cover the widened frame. |

**`G_EX_ASPECT_STRETCH` means "do not squeeze this to 4:3", not "cover the
widened frame".** With it, a 320-wide rectangle reaches the frame's full width and
a narrower one is widened about its centre by the same factor. Pinning an
element's own edges to the frame's edges is a different operation -- that is what
the extended origins do, and for `stretch` it was tried and reverted (the comment
at the `Class::Stretch` case in `src/dlrewrite.cpp` says why). This distinction
costs people days; see PORTING.md §7, *Widescreen 2D*.

### A worked example: fixing an element

1. Get the game to the screen with the problem. If it is a transition, tick
   **Hold this frame** while it is on screen.
2. Hover down the list until the yellow outline lands on the thing you want to
   fix. Click that row to pin the outline.
3. Note the `identity`. That is the answer the screenshots could never give.
4. Change the dropdown and watch the screen. The next frame uses the new class.
5. When it looks right, press **Save to hud.json**.
6. If the element belongs to the game rather than to your taste, move the
   identity out of `hud.json` and into the built-in table in `src/dlrewrite.cpp`
   (`by_identity["tex:0x01005748"] = Class::Stretch;`) so every player gets it.

### Reading the outline

The outline is the rewriter's arithmetic run forwards. Window `W` x `H`, with the
4:3 box `B = H * 4/3`:

| Class | Horizontal placement of game pixel `x` |
|---|---|
| `center` | `(W - B)/2 + x/320 * B` |
| `left` | `x/320 * B` |
| `right` | `W - B + x/320 * B` |
| `stretch` | `x/320 * W` |

Vertically always `y/240 * H`. It is an approximation: a scissor can still cut an
element short, and RT64's upscaling filters move edges by a pixel. It is accurate
enough to point with, which is all it is for.

### What RT64 already does

Do not look for these in the port's window -- they are in the **Game editor**
window that appears with it, and they are better than anything a port would
write:

| Question | Where |
|---|---|
| Freeze the game entirely, keep the frame interactive | *Debugger* tab, pause |
| What drew this pixel? | Right-click the pixel; it lists the draw calls under it and highlights one |
| What is in this framebuffer? | *Debugger* tab, Framebuffers |
| What does this texture look like? | *Textures* tab |
| Turn widescreen, upscaling or filtering off to isolate a fault | *Configuration* tab |

The port's window answers only the half RT64 cannot: **which of the game's
elements this is, what the port decided about it, and what happens if that
decision changes.**

---

## Putting it in another project

Five pieces. Only the first is RT64-specific.

### 1. A hook in RT64, because RT64 owns the ImGui context

A port cannot call `ImGui::Begin` of its own: RT64 creates the context, begins
the frame and ends it, and an ImGui call outside that window either asserts or
draws nothing. So add one function pointer that RT64 calls from inside its own
frame. In `src/hle/rt64_state.cpp`, before `namespace RT64 {`:

```cpp
extern "C" void (*RT64_PortInspectorHook)() = nullptr;
```

and in `State::inspect()`, after `debuggerInspector.checkPopup(workload)`:

```cpp
if (RT64_PortInspectorHook != nullptr) {
    RT64_PortInspectorHook();
}
```

That is the entire patch. It is null unless a port sets it, so upstream behaviour
is unchanged, and it costs one predictable branch per frame, in developer mode
only.

**Script the patch.** RT64 is a submodule; an edit made by hand is reverted
without warning by the next `git submodule update`. This project's is
`tools/patch_rt64_inspector.py`: idempotent (it looks for its own symbol first),
and it exits with a clear message if the anchor text has moved upstream. It is
chained into `tools/patch_rt64.py`, because the port **links** against the symbol
whether or not the inspector is ever switched on -- a fresh clone that skipped it
would fail at link time rather than at runtime.

Check the two anchors against your RT64 revision. They are stable points, but
they are still text.

### 2. Turn developer mode on and leave it on

RT64 gates **four** things on `userConfig.developerMode`, and every one of them
is on the path between the F1 key and the window:

| Gate | File |
|---|---|
| `usesWindowMessageFilter()` -- whether RT64 installs its SDL event filter and Win32 subclass at all | `rt64_application_window.cpp:398` |
| `sdlEventFilter()` / `windowMessageFilter()` -- whether F1 is looked at | `rt64_application.cpp:556`, `:590` |
| `processDeveloperShortcut(Inspector)` -- whether the keystroke creates the inspector | `rt64_application.cpp:628` |
| `State::inspect()` -- whether any of it is drawn | `rt64_state.cpp:1638` |

So there is no halfway position: the port turns it on unconditionally, at
construction, before `setup()`.

```cpp
// The frontend build, src/frontend.cpp
(void)developer_mode;
wr64::inspector::install();
return std::make_unique<RewritingContext>(
    rdram, recompui::renderer::create_render_context(
               rdram, window_handle, presentation_mode(), true));
```

```cpp
// The runtime-only build, src/renderer.cpp -- same idea, RT64 direct
app_ = std::make_unique<RT64::Application>(core, app_config);
app_->userConfig.developerMode = true;
const RT64::Application::SetupResult result = app_->setup(window_handle.thread_id);
```

`install()` must run before the first frame and is a no-op when the port's half
is disabled, so it sits there unconditionally.

**What it costs:** a mutex lock and a null check per frame while the menu is
closed, plus a handful of `if (warningsEnabled)` branches in the RDP command
handlers that only build a string when a command is genuinely malformed
(`rt64_rdp.cpp:617`). Nothing in the draw path. `set_application_user_config()`
does not touch `developerMode`, so changing a graphics setting will not undo it.

The frontend's own developer-mode checkbox in the graphics tab is left to mean
whatever else it means -- it no longer decides whether the debug menu opens, and
a reader should never have to find it.

**Then audit the other shortcuts.** Developer mode is a bundle: turning it on
for everyone arms every key RT64 binds behind it, not just the one wanted. Go
through them and decide each on its own. Here F2 (a session-wide ray tracing
toggle, invisible in any menu, unexplained on screen) was removed and F3 and F4
kept, because both of those are visibly reversible and plainly diagnostic. The
same script does it -- removing the `case` leaves the key unfiltered, so it
reaches the game like any other.

### 3. The threading contract

Two threads meet, and this is the part to get right.

| Thread | Does |
|---|---|
| Display-list thread | Runs the classifier. Fills a *frame under construction*, one entry per classified element. |
| Renderer UI thread | Draws the panel. Reads only the *published* frame. |

```
begin_frame()  -> clear the frame under construction
note_element() -> append, on every classified element
end_frame()    -> swap it into the published slot, under a mutex
```

Only `end_frame` takes the lock on the classifier's side, so the per-element cost
is a `push_back` on a vector that has already reached its size. The panel copies
the published frame once per UI frame and then works from the copy, so it never
holds the lock while ImGui is running.

Cap the element count (this project uses 512). A pathological display list must
not be able to grow the vector without bound behind the panel's back.

The override table is the one thing read on the classifier's *hot* path -- every
element of every frame -- so keep it small and check the cheap condition first:

```cpp
bool override_class(const char* identity, int* out_class) {
    if (!g_enabled || identity == nullptr) return false;
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_overrides.empty()) return false;          // the usual case
    ...
}
```

### 4. Five calls in the classifier

That is all the instrumentation costs. In this project, `src/dlrewrite.cpp`:

| Call | Where |
|---|---|
| `begin_frame(state)` | Top of the per-frame rewrite, with whatever the game's state variable is |
| `note_element(...)` | In each classifier -- here one for rectangles (`classify_rect`) and one for triangle runs (`classify_draw`) |
| `end_frame()` | After the display list has been walked |
| `override_class(...)` | **First** in the classification chain, ahead of the on-disk tag table and the built-in table, so a change in the panel wins immediately and can be undone without a restart |

The override path has to map back into the classifier's own enum, and honour the
same preconditions the normal path does -- an anchored class is meaningless for an
element the rewriter cannot anchor:

```cpp
int chosen = 0;
if (wr64::inspector::override_class(identity.c_str(), &chosen) ||
    (!identity2.empty() && wr64::inspector::override_class(identity2.c_str(), &chosen))) {
    switch (chosen) {
        case wr64::inspector::kLeft:    return anchors ? Class::Left : Class::Auto;
        case wr64::inspector::kRight:   return anchors ? Class::Right : Class::Auto;
        case wr64::inspector::kStretch: return Class::Stretch;
        default:                        return Class::Auto;
    }
}
```

Keep the interface's class enum a plain `int`, so the classifier's own enum stays
private to it and the two files never have to include each other.

### 5. The panel

Ordinary ImGui, drawn from the hook. What is worth copying from
`src/inspector.cpp`:

- **A table, not a list.** `ImGuiTableFlags_ScrollY | SizingStretchProp`, with the
  identity column `WidthStretch` and the rest fixed. An identity truncated to
  `tex:0x08…` is worthless, and that is exactly what a naive layout gives you.
- **Row selection via `Selectable` with `SpanAllColumns | AllowItemOverlap`** in
  the first column, so the whole row is a hover and click target while the
  dropdown in the last column still works.
- **Outlines on `ImGui::GetForegroundDrawList()`**, so they draw over the game and
  over every ImGui window. Map game coordinates to the viewport with
  `ImGui::GetMainViewport()`'s `Pos` and `Size` -- see
  [Reading the outline](#reading-the-outline).
- **Compute the effective class once, at the top of the row**, before drawing any
  column. The outline and the dropdown have to agree, and they will not if each
  looks the override up separately.
- **A `Hold` copy separate from the published frame.** RT64's pause freezes the
  game; hold freezes only the list. Both are wanted, for different faults.
- **Persist to the same file the classifier reads at startup**, merging rather
  than overwriting, and remove an identity from the other lists before adding it
  to one -- otherwise changing your mind in the panel leaves both answers on disk.

### If your renderer is not RT64

Only piece 1 changes. Anything that gives you a per-frame callback with a UI
context already open will do; the port's side is renderer-agnostic. What does not
survive the swap is the outline arithmetic in
[Reading the outline](#reading-the-outline) -- that is RT64's widescreen model,
and you would substitute your own.

---

## Files

| File | What |
|---|---|
| `include/wr64/inspector.h` | The interface, and the threading contract written down |
| `src/inspector.cpp` | The panel, the frame double-buffer, the override table, the save |
| `tools/patch_rt64_inspector.py` | The RT64 hook. Idempotent; chained into `tools/patch_rt64.py` |
| `src/dlrewrite.cpp` | The five call sites |
| `src/frontend.cpp` | `install()`, and forcing developer mode on so F1 works |
| `src/renderer.cpp` | The same, for the runtime-only build |
| `src/main.cpp` | `init()` |

---

## Keeping this current

This file describes a tool that lives in the port; if the tool changes, change it
here in the same commit. The reasons behind the design belong in
[PORTING.md](PORTING.md) §7 and the game's own facts in
[GAME-INTERNALS.md](GAME-INTERNALS.md). This file is the manual and the recipe.
