# Subject/Observer rearchitecture — design

Prototype on branch `proto/subject-signals`, based on `upstream/master` @ 9f4e962f5.

Goal: give `lv_subject_t` autowired dependencies, per-subject mapper functions, and
eager/lazy evaluation. The reactive model was inspired by Angular signals; that is
acknowledged once in `lv_observer.h` and nowhere else, because an embedded C developer
should not need to know how those work.

## 1. The model

### 1.1 One mapper per subject, and it owns the value

A mapper is the only thing that writes a subject's value. It receives:

| Parameter | Meaning |
|---|---|
| `subject` | the subject being evaluated |
| `user_data` | the mapper's captured state, like a closure's captured variables |
| `input` | the value most recently written with `lv_subject_set_...()` |
| `value` | in/out pointer to the stored value, holding the **previous** one on entry |

`lv_subject_set_...()` records the input. An **eager** subject then maps and notifies
before returning; a **lazy** one stops there and maps when something reads it, when an
eager dependent pulls it, or at the next flush. Every getter pulls, so a read is never
stale, and a lazy subject nothing reads is never evaluated at all.

Deferral applies to the scalar input types. Three cases cannot defer and always map on
the spot: one with an immediate observer (promoted anyway), one that owns its value
(ownership of an incoming pointer only transfers once the mapper stores it, so deferring
would let a second write supersede an input that nothing would ever release), and one
whose input is pointer-shaped — `POINTER` or `STRING`. A pointer input is borrowed: valid
only for the duration of the write, and the mapper may read *through* it. Deferring cost
a stack-use-after-scope that the clamped-subject test caught.

A subject's input type need not be the type of its value
(`lv_subject_create_mapped()`), which is why the mapper's `input` is
`lv_subject_value_t`: one mapper family, keyed by the value type, has to serve every
input type. The clamped helper uses this — its input is an `lv_subject_range_t` pointer,
so its bounds are writable at run time.

Because a lazy subject only ever sees the last value written, a lazy mapper must be pure
and must not accumulate. That is why the extremum helpers are eager.

The mapper owns change detection: it returns whether it changed the stored value. Since
`*value` holds the previous value on entry, keeping a local copy is all that is needed.
That is why the subject stores **no history**: `prev_value` and every
`lv_subject_get_previous_*()` are gone, and a string subject needs only one buffer.

`input` is the last *written* value even on a dependency-driven re-evaluation, so a
clamp whose limits widen restores a value it had previously clamped away.

A mapper **must not write a subject**. A write from inside one is ignored with a
warning. This is what makes the drain terminate without a round cap: an evaluation
cannot re-dirty anything. Observer callbacks may write freely — that is where side
effects belong.

### 1.2 Modes on both subjects and observers

| | Subject | Observer |
|---|---|---|
| eager / immediate | recomputes and notifies inside `set()` | notified inside `set()`; promotes its subject to act eager |
| lazy / batched | only marked dirty | notified once per `lv_subject_flush()`, i.e. once per frame |

Defaults are subject **LAZY** and observer **IMMEDIATE**, which reproduces the old
behavior exactly for unchanged code. An eager subject notifies all of its observers,
batched ones included.

A read brings the value up to date but deliberately does **not** deliver pending
notifications; otherwise any reader would break the per-frame batching.

### 1.3 Evaluation is two-phase and glitch-free

A write first marks the transitive dependent closure dirty, then evaluates the
effectively-eager subjects, whose mappers pull their dependencies. In a diamond
(A→B, A→C, B+C→D) `D` evaluates exactly once and never sees a fresh `B` beside a stale `C`.

### 1.4 Subjects needing a scan are a prefix of the global list

`LV_GLOBAL_DEFAULT()->subject_ll` already held every `lv_subject_create()` subject.
Invariant: every subject a drain or a flush must visit sits in a prefix of it, so a walk
starts at the head and stops at the first subject that needs no scanning — O(pending),
not O(all). `lv_ll_move_before()` relinks without reallocating, so subject pointers stay
valid across a move.

Note "needs a scan" rather than "is dirty": a subject that is dirty but has no observers
and is not eager is due for nothing, and is deliberately kept out of the prefix. See
§1.7.

### 1.5 Deleting

`lv_subject_delete()` refuses while another subject's mapper reads the target, because
that would leave the mapper without an input. It returns `LV_RESULT_INVALID` when it
refuses and `LV_RESULT_OK` when it deletes, so the caller can tell whether it still owns
the subject. `lv_subject_delete_cascade()` takes the transitive dependents with it, like
SQL's `ON DELETE CASCADE`. `lv_deinit()` cascades from the head one at a time, so no
ordering has to be worked out.

`lv_subject_set_delete_cb()` is how an application that keeps its own pointers hears
about any of these paths. That is the difference the demo shows: a mapper input cannot be
deleted from under its reader, while a plain subject that observers compute can, and then
every pointer to it is the application's own problem.

### 1.6 Pointer values

A pointer subject notifies on every write, because the data behind an unchanged pointer
may have been rewritten. Two answers:

- **Mutation detection**: a mapper keeps a last-known-good copy in its `user_data` and
  compares contents. No new API was added for this; the captured state is enough.
- **Ownership, decided per write.** `lv_subject_set_pointer()` borrows,
  `lv_subject_set_pointer_owned()` transfers. Strings get the same pair
  (`lv_subject_set_string()` / `_owned()`) for the no-buffer form; the buffer form keeps
  copying and has no lifetime question. A pointer is released once neither the stored
  value nor the retained input refers to it, which is what makes republishing the same
  pointer safe. The release is strictly after the mapper runs. Ownership follows what the
  write handed in: a pointer a mapper produced itself is treated as borrowed.

- **Copying**, the third form. `lv_subject_set_buffer()` gives a subject storage, fixed
  or growable, and `lv_subject_copy_pointer()` / `lv_subject_copy_string()` copy into it.
  No lifetime question, and change detection comes for free because the previous bytes
  are still there to compare against — which solves the mutation-behind-a-pointer problem
  without a mapper. A write that does not fit is refused, not truncated: a fixed buffer
  too small, or a `realloc_cb` returning NULL, fails the write and keeps the old value.
  `lv_subject_copy_string_trimmed()` stores as much as fits instead, and is what users
  are pointed at for on-screen text; it is also the migration path for code that relied
  on the old silent clipping. There is deliberately no pointer equivalent, because half a
  struct is not a shorter struct.
  The buffer descriptor is a separate allocation, so `lv_subject_t` pays one pointer
  rather than five fields for subjects that never copy.

A borrowed pointer has to stay valid until a **new input is written**, not merely until
the call returns, because the input is retained and handed to the mapper again on every
re-evaluation. Retaining it is deliberate — it lets a mapper re-derive from its input —
and it is why the lifetime rule is stated in terms of the next write.

### 1.7 Two things that keep the cost down

**A no-change stops the propagation.** Every subject carries a `version`, bumped only
when its value really changes, and every dependency edge remembers the version it read.
Before re-running a dirty subject's mapper, its dependencies are brought up to date and
their versions compared: if they all still match, the mapper is a function of unchanged
inputs and is skipped. Because skipping leaves this subject's own version alone, its
dependents skip too, transitively. A value that settles back to what it was therefore
costs one evaluation, not one per level.

The trap here, which the tests caught: a *deferred* write also arrives through the
recompute path, and then the pending work is the input itself rather than a stale
dependency. An `input_pending` flag distinguishes the two, and the mapper always runs
when it is set.

**Never-due subjects leave the scanned region.** A subject that is dirty but has no
observers and is not eager is due for nothing: no drain evaluates it, no flush evaluates
it, and it is computed when something reads it. Such subjects otherwise pile up at the
front of the list and every drain walks past all of them, so the cost of a write grows
with the number of derived subjects nobody reads. `needs_scanning()` keeps them in the
tail region instead. Anything that can make one due again — gaining an immediate
observer, or being declared eager — repositions it first.

### 1.8 Removed

- `LV_SUBJECT_TYPE_GROUP` and its three functions. An `LV_SUBJECT_TYPE_NONE` subject with
  a mapper expresses the same thing and more: the mapper decides *when* to notify, the
  member set can vary per run, and members are real dependencies so the delete rule
  protects them. Enum value 6 is left unused.
- `min_value` / `max_value` and their four setters, replaced by a clamping mapper (in
  place) or `lv_subject_create_clamped()` (a separate bounded mirror).
- `prev_value` and the five `lv_subject_get_previous_*()`.

### 1.8b A mapper writes only its own output

An observer's mapper now receives `(observer, input, *out)`, symmetric with a subject's
mapper: `input` is the subject's current value, `*out` is the observer's own output slot
holding what it last pushed. Before, an observer mapper had no `input` and had to fetch
the value through `lv_observer_get_subject()`, which read as though `*out` were the
input — it was misread exactly that way, which is why it changed.

The rule that comes with it: **a mapper may write only its own output.** Never through
`input`, never through any pointer it reads. Two tests pin why:

- On the observer side the leak is immediate and ordered. Observers are notified in the
  order they were added, so a mapper that mutates the value is seen by every observer
  after it. `test_observer_mapper_mutation_is_seen_by_later_observers` asserts the second
  observer sees `999` rather than what was published.
- On the subject side it is the same hazard but the timing differs, and testing it
  corrected an assumption of mine. Dirty subjects are evaluated from the head of the
  global list and marking moves them there, so derived subjects run roughly in reverse
  registration order — the reader happened to run *before* the mutator, and saw the clean
  value. What is unambiguous, and what the test asserts, is that a consumer corrupted the
  value its producer published, and that the corruption outlives the update: a lazy
  dependent read afterwards gets `555`, not the published `1`.

So it is not an observer-only rule. Writing a subject's *own* buffer in copy mode is
fine, because the buffer belongs to the subject.

### 1.9 An observer has one pointer for application data

An observer used to carry two general-purpose pointers, `target` and `user_data`, doing
the same job. `target`'s generic role is gone, along with
`lv_subject_add_observer_with_target()` and `lv_observer_get_target()`; `user_data` is
the single place for application data.

The widget slot stays, because it is not data. It is a **lifetime link**: the widget's
deletion deletes the observer, and `lv_observer_delete()` needs the widget pointer to
unhook that event. Merging it into `user_data` would relocate the special case rather
than remove it, and would cost the ability to have both — which several of LVGL's own
bindings need, `lv_obj_style.c` among them, keeping a descriptor in `user_data` and
reaching the widget separately.

So the struct lost one pointer and one flag bit (`for_obj`), and the API lost two
functions, with no capability lost.

### 1.10 Releasing what the application attached

`lv_subject_set_delete_cb()` runs a callback just before a subject is destroyed, on every
path that destroys one. `lv_subject_set_mapper_user_data_owned()` is the one-call form
for the common case, and needs no new storage because it reuses the ownership bit the
`lv_subject_create_min/max/clamped()` helpers already used internally.

`lv_subject_set_external_data()` also released on deletion, but it sits behind
`LV_USE_EXT_DATA`, which is off by default, and it is a separate slot from the mapper's
`user_data` — so it did not answer the question it looked like it answered.

### 1.9 Forwarding a subject to a widget setter

`lv_obj_bind_int()` and friends already forward to a setter shaped
`void (lv_obj_t *, value)`. The gap was setters that take more: 129 style setters shaped
`(obj, value, selector)`, plus the handful with an animation flag.

Two mechanisms, because the trade-off differs:

- `lv_obj_bind_style_int/color/opa()` for the style families. A single expression, and
  the selector rides in the observer's output slot, which is otherwise only used by a
  mapped observer, so nothing collides and nothing is allocated.
- `LV_SUBJECT_FORWARD_INT/FLOAT/STRING/COLOR/POINTER(name, setter, ...)` for everything
  else. The macro expands to a `static` observer callback that calls the setter directly,
  so it is type-checked and free at run time, and it handles any signature.

A function-pointer-cast approach — storing one extra argument as `intptr_t` and casting
the setter to a different signature — was rejected: calling through an incompatible
function pointer type is undefined behaviour, and the macro gets the same result with
none.

## 2. Decisions I made rather than asked

- The mapper's `subject` parameter is kept alongside `user_data`, so a mapper can read
  its own identity and type.
- `lv_subject_value_t` appears in the compare callback and the clamp bounds, because one
  callback has to serve every subject type. It stays out of the mapper contract.
- Observer mappers do **not** autowire: an observer is not a node in the graph and runs
  only when its own subject notifies. They reach state through the observer's `user_data`.
- Observer mappers are installed at bind time (`lv_obj_bind_*_mapped()`), because binding
  notifies immediately and a later-installed mapper would miss that first push.
- A string subject does not retain its last *input*; `input.pointer` is NULL on a
  re-evaluation. Note this is about the input, not the value: the value is in the buffer
  and is very much retained.

  Two reasons, and the first is the blocker: `lv_subject_snprintf()` formats into a
  temporary that it frees before returning, so keeping that pointer would leave a
  dangling one. And it is not needed, because `lv_subject_copy_string()` copies into the
  buffer, so a mapper re-deriving on a dependency change reads the last value from `buf`.

  A pointer input, by contrast, is retained, because there is nothing equivalent to fall
  back on — the subject stores the pointer itself, not a copy of what it refers to.
- The flush is a lazily created period-0 `lv_timer`, paused whenever nothing is pending.
  It is not a hook inside `lv_timer_handler()` because `lv_subject_global_init()` runs
  before `lv_timer_core_init()`, and a lazily created timer costs nothing in a project
  that never uses a lazy subject.

## 3. Traps handled

- `lv_subject_get_*` registers a dependency whenever a mapper is running. Observer
  callbacks read subjects throughout LVGL widget code, so notification saves, clears and
  restores the "currently evaluating" pointer — as a local, since a pulled dependency may
  notify inside an outer evaluation.
- Subscribing has to settle the subject first: pull a stale value **and** deliver a
  pending notification to the existing observers, both before linking the new observer.
  Skipping the second cost a full debugging session — a sysmon notification left pending
  from a time when the subject had no eager observers surfaced at an arbitrary later
  flush and invalidated a widget on a different display.
- A subject set up with a deprecated `lv_subject_init_*()` is not in the global list, so
  it must never be relinked. Its mapper is never re-evaluated by a flush, and setting one
  warns.

## 4. Verification

- `python3 tests/main.py test --build-options OPTIONS_TEST_DEFHEAP` — 183/183 pass.
- `python3 tests/main.py build` — every configuration compiles, including
  `OPTIONS_MINIMAL` with `LV_USE_OBSERVER 0`, and the examples.
- `test_observer.c` covers 184 cases.

Measured coverage of `src/core/lv_observer.c` (gcov, `OPTIONS_TEST_DEFHEAP`):

| | before the audit | after |
|---|---|---|
| lines executed | 78.8% | **93.7%** |
| branches executed | 81.4% | **98.0%** |

The audit that produced those numbers also found that the subject-changing event helpers
(`lv_obj_add_subject_increment_event`, `..._toggle_event`, `..._set_int/float/string_event`)
had **never** been covered — not before this work either — while this rearchitecture
changed their behaviour: the increment event used to intersect its own range with the
Subject's `min_value`/`max_value`, and those fields are gone. A documented behaviour
change with no test was the largest real gap, and it is now covered including the
interaction with a clamping mapper.

What is deliberately still uncovered, about 120 lines:

- allocation-failure and argument-check paths, which need fault injection to reach;
- the deprecated `lv_subject_init_float/pointer/color/string()`, which are removed in
  v10, so testing them would be maintaining code on its way out;
- the `dependents` loop in `edges_teardown()`, which `lv_subject_delete()` now refuses to
  reach and `lv_subject_delete_cascade()` empties first. It is defensive, and worth
  keeping as such.

Two habits the audit was worth for, beyond the numbers. Three tests leaned on ASan to
catch a dangling edge rather than asserting anything, so they now compare the global
Subject count before and after a cascade — which also pins that a diamond's sink is
deleted exactly once. And one pair of clamped-Subject tests turned out to be a strict
subset of the other, so the weaker one is gone; the other near-duplicate names checked
out as genuinely different paths, direct-write deferral against dependency-driven
deferral, and were kept.

## 5. How this could fit LVGL XML and the Pro Editor

A sketch, not implemented.

### 5.1 The principle

Most real mappers are one of a handful of shapes, and those shapes can be declared.
Beyond them, a mapper is pure computation over subject values — so it can be *generated*
from a small expression language rather than hand-written (§5.5). Anything past that
stays a named C hook.

So there are three tiers, and each is cheaper for the editor than the one after it:

| Tier | Declared as | Editor sees |
|---|---|---|
| Common shapes | `<clamp>`, `<min>`, `<max>`, `<format>`, `<depends>` | semantics and dependencies |
| Computation | `<expr>` | dependencies, and can validate |
| Everything else | `<mapper name="..."/>` | an opaque node |

The payoff is that a declared graph is statically known. The editor can draw it, validate
it and show live values without running anything.

### 5.2 Declarative subjects

`globals.xml` already declares subjects. Extend it with the shapes that carry their own
semantics:

```xml
<subjects>
    <!-- a plain source, as today -->
    <int name="volume" value="50"/>

    <!-- bounded in place: generates a clamping mapper -->
    <int name="brightness" value="128">
        <clamp min="0" max="255"/>
    </int>

    <!-- derived: running extremum of another subject -->
    <int name="volume_peak">
        <max of="volume"/>
    </int>

    <!-- derived: a separate bounded mirror -->
    <int name="volume_bar">
        <clamped of="volume" min="0" max="100"/>
    </int>

    <!-- derived string, the format-string case -->
    <string name="volume_text">
        <format of="volume" fmt="%d%%"/>
    </string>

    <!-- an aggregate: LV_SUBJECT_TYPE_NONE plus a generated mapper -->
    <none name="audio_changed" mode="eager">
        <depends on="volume brightness volume_peak"/>
    </none>

    <!-- anything else: a named C mapper, declared but not described -->
    <int name="loudness">
        <mapper name="loudness_mapper" user_data="loudness_cfg"/>
    </int>
</subjects>
```

`mode="lazy|eager"` maps to `lv_subject_set_mode()`. The `<mapper>` element is the escape
hatch: the codegen emits `lv_subject_set_int_mapper(&loudness, loudness_mapper, &loudness_cfg);`
and the editor treats `loudness_mapper` exactly as it already treats an event callback
name — a symbol the project must provide, surfaced as an unresolved reference until it does.

### 5.3 Bindings

Widget bindings already name a subject. An observer mapper is the same kind of hook:

```xml
<lv_arc bind_value="pressure" bind_value-mapper="clamp_to_arc_range"/>
<lv_label bind_text="volume" bind_text-fmt="%d%%"/>
```

`bind_text-fmt` is really a built-in observer mapper already, which is a good sign the
concept fits the existing syntax rather than fighting it.

### 5.4 What the editor gains

- **A graph view.** Sources, derived subjects and widget bindings are nodes; dependencies
  and bindings are edges. Because the declarative subset states its dependencies, the
  graph is known without executing anything. A `<mapper>` node is drawn as opaque, with
  its dependencies discovered at run time in preview.
- **Live values in preview.** The runtime already knows every subject
  (`lv_subject_get_dependency_count()` / `lv_subject_get_dependency()` expose the edges),
  so the editor can overlay current values and highlight what just changed.
- **Static validation** the editor can do before running: a cycle in the declared graph; a
  write to a derived subject; a lazy subject with no reader and no observer, which will
  never be computed; a subject deleted while something depends on it.
- **Eager/lazy made visible.** Colour nodes by mode and mark the frame boundary, so the
  cost of an eager chain is visible while editing rather than at run time.

### 5.5 A primitive expression language for mappers

My first take on this was "no expression language, use a named C mapper for anything past
the declarative shapes". That was wrong, and the argument against it is the interesting
one.

**A mapper's only interaction with the world is reading subject values and computing a
result.** It takes no callbacks, performs no I/O, and is forbidden from writing a
subject. So a small expression language does not need an interpreter, a runtime, or any
reflection: it can *generate the C mapper body directly*, and the generated code is
indistinguishable from what a person would write.

```xml
<subjects>
    <int name="width"  value="10"/>
    <int name="height" value="4"/>

    <int name="area">
        <expr>width * height</expr>
    </int>

    <int name="level">
        <expr>temp &gt; 80 ? 2 : temp &gt; 60 ? 1 : 0</expr>
    </int>

    <int name="bounded">
        <expr>clamp(raw, 0, 100)</expr>
    </int>
</subjects>
```

generating, for `area`:

```c
static bool area_mapper(lv_subject_t * subject, void * user_data, lv_subject_value_t input,
                        int32_t * value)
{
    LV_UNUSED(subject);
    LV_UNUSED(user_data);
    LV_UNUSED(input);

    int32_t next = lv_subject_get_int(width) * lv_subject_get_int(height);
    if(next == *value) return false;
    *value = next;
    return true;
}
```

Why this fits unusually well:

- **The dependency graph builds itself from the generated reads**, exactly as it does for
  hand-written C. The codegen does not have to emit a dependency list, and cannot get one
  wrong.
- **Generated mappers are pure by construction.** The language has no way to write a
  subject or call out, so every generated mapper is safe as `LV_SUBJECT_MODE_LAZY`. That
  matters because purity is the one part of the contract a human can quietly violate —
  the accumulating-mapper example exists precisely because of it. An expression can't.
- **Change detection is uniform.** Compare-and-return is emitted the same way every time,
  so the "returns true unconditionally and notifies on every write" mistake disappears.
- **The editor gets an exact graph and live values** with no extra machinery, because the
  generated code is ordinary code.

Scope worth holding to:

| In | Out |
|---|---|
| `+ - * / %`, comparisons, `&& \|\| !`, ternary | assignment, sequencing, loops |
| `clamp min max abs map` | anything with a side effect |
| subject names, numeric and enum literals | pointer arithmetic, casts |
| string results via a `format` shape | arbitrary string manipulation |

Details that need a decision rather than a guess: integer division by zero (generate a
guard, or refuse at codegen when the divisor is a literal zero) and whether comparison
results are `0/1` ints or a distinct bool type. None is hard; all are choices worth
making once and writing down.

Int/float promotion is settled. A Subject of either type can be read and written as the
other, so an expression and a widget both stop caring which one was declared. An `int`
read as a `float` is exact; a `float` read as an `int` follows the Subject's
`lv_subject_set_rounding()` mode, which rounds to the nearest by default and can
truncate instead. That is what removed the `if(type == INT) … else …` pair from every
widget that binds a value.

`LV_SUBJECT_ROUND_EXACT` is the strict mode: it requires every conversion to be
lossless, refuses a write that is not, and warns on a read. Both directions can lose
something — a float carrying a fraction, and an `int32_t` past 2^24 that a float's
24-bit mantissa cannot hold — and it covers both. A read cannot be refused, since the
getter returns the value, so `lv_subject_get_int_checked()` and
`lv_subject_get_float_checked()` report it through an `lv_result_t` instead.

A recursive-descent parser over that grammar is a few hundred lines in the existing
Python codegen, and the `<mapper name="..."/>` escape hatch stays for whatever the
language deliberately cannot say.

### 5.6 What to leave out

- **Value ownership.** It only matters for pointer subjects produced at run time. XML
  subjects are statically declared globals; keep ownership a C-only concern.
- **Anything with a side effect.** An eager subject's mapper may have side effects in C,
  but an XML expression should not be able to express one. Side effects belong in an
  observer the application writes.

## 6. Low-hanging fruit for LVGL XML

The reason there is so much of it: **the runtime now does the hard part.** Dependency
discovery, change detection, and scheduling are all handled by the C side, and a mapper's
only interaction with the world is reading subject values. So XML has nothing to
orchestrate — it only has to *declare*, and the codegen only has to *emit reads*.

Ordered by effort, cheapest first.

### 6.1 No codegen at all: XML that calls existing API

These need a parser change and one generated call each. No new C, no expression
evaluation, nothing to get subtly wrong.

| XML | Generates |
|---|---|
| `<int name="v" mode="eager"/>` | `lv_subject_set_mode(v, LV_SUBJECT_MODE_EAGER)` |
| `<int name="bounded"><clamped of="raw" min="0" max="100"/></int>` | `lv_subject_create_clamped(raw, …)` |
| `<int name="peak"><max of="v"/></int>` | `lv_subject_create_max(v, NULL)` |
| `<int name="lowest"><min of="v"/></int>` | `lv_subject_create_min(v, NULL)` |
| `<lv_obj bind_style_bg_color="col" bind_style_part="main"/>` | `lv_obj_bind_style_color(obj, col, lv_obj_set_style_bg_color, LV_PART_MAIN)` |
| `<lv_label bind_text="v" bind_mode="batched"/>` | the bind, plus `lv_observer_set_mode(…, BATCHED)` |

The style-binding row is worth calling out: there are 129 style setters, all one shape,
so a single attribute pattern covers the lot.

### 6.2 One generated function, no expression parsing

`<depends on="hour minute format"/>` on an `LV_SUBJECT_TYPE_NONE` subject generates a
none-mapper that reads each named subject and returns `true`. Ten lines of codegen, and
it replaces the whole Group Subject concept.

`<format of="volume" fmt="%d%%"/>` on a string subject generates a string mapper that
formats and compares. Slightly more, because it has to pick the right `lv_subject_get_*`
per operand type, which the subject declarations already give it.

### 6.3 The expression language (§5.5)

The largest of the three and still not large, because it generates straight-line C over
value reads. Worth doing after 6.1 and 6.2, since those cover most real subjects and
prove the plumbing first.

### 6.4 What the editor gets for free at each step

- After 6.1: modes and derived-subject shapes are visible and editable, and the graph
  has real edges for every `of=` and `bind_*` attribute.
- After 6.2: aggregates and derived strings appear as nodes with known dependencies.
- After 6.3: **generated mappers are pure by construction**, so the editor can promise
  that a declared subject is safe as lazy. That is the one part of the contract a hand
  written mapper can quietly break — the accumulating-mapper example exists because of
  it — and an expression cannot express the violation.

At every step, `lv_subject_get_dependency_count()` / `_get_dependency()` let the editor
read the live graph back out of a running preview, so the diagram and the program cannot
disagree.

### 6.5 The gap worth closing first

The graph is only readable in one direction. `deps` is exposed; `dependents` is not, even
though the reverse edges are right there in the struct. Two accessors would let the
editor answer "what breaks if I delete this subject?" — which is also exactly the
question `lv_subject_delete()` now refuses on, so the runtime and the editor would be
answering it from the same data.

## 7. Not low-hanging

- **Writing subjects from an ISR or another thread.** Needs atomic stores with
  release/acquire ordering, dirty propagation moved out of the write path, and the flush
  walking the whole list. A design, not a tweak; see §1 for what a write currently
  touches.
- **A "remember the previous value" flag.** Not needed at all, and proposing it was a
  lapse: a mapper is handed the previous stored value in `*value`, and its `user_data` is
  a place to keep it, so "I need to know what it was" is a **stateful mapper**, not
  missing API. Better than a flag, in fact, because the state is per subject rather than
  per file — `test_subject_stateful_mapper_is_per_subject` shows two subjects tracking
  their own transitions through one mapper, which a file-scope static cannot do.
