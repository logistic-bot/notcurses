% notcurses_init(3)
% nick black <nickblack@linux.com>
% v1.4.3

# NAME

notcurses_init - initialize a notcurses instance

# SYNOPSIS

**#include <notcurses/notcurses.h>**

```c
#define NCOPTION_INHIBIT_SETLOCALE 0x0001

typedef struct notcurses_options {
  const char* termtype;
  bool inhibit_alternate_screen;
  bool retain_cursor;
  bool suppress_banner;
  bool no_quit_sighandlers;
  bool no_winch_sighandler;
  FILE* renderfp;
  int margin_t, margin_r, margin_b, margin_l;
  unsigned flags; // from NCOPTION_* bits
} notcurses_options;
```

**int notcurses_lex_margins(const char* op, notcurses_options* opts);**

**struct notcurses* notcurses_init(const notcurses_options* opts, FILE* fp);**

# DESCRIPTION

**notcurses_init** prepares the terminal for cursor-addressable (multiline)
mode. The **FILE** provided as **fp** must be writable and attached to a
terminal, or **NULL**. If it is **NULL**, **/dev/tty** will be opened. The
**struct notcurses_option** passed as **opts** controls behavior. Only one
instance should be associated with a given terminal at a time, though it is no
problem to have multiple instances in a given process.

On success, a pointer to a valid **struct notcurses** is returned. **NULL** is
returned on failure. Before the process exits, **notcurses_stop(3)** should be
called to reset the terminal and free up resources.

An appropriate **terminfo(5)** entry must exist for the terminal. This entry is
usually selected using the value of the **TERM** environment variable (see
**getenv(3)**), but a non-**NULL** value for **termtype** will override this. An
invalid terminfo specification can lead to reduced performance, reduced
display capabilities, and/or display errors. notcurses natively targets
24bpp/8bpc RGB color, and it is thus desirable to use a terminal with the
**rgb** capability (e.g. xterm's **xterm-direct**).

If the terminal advertises support for an "alternate screen" via the **smcup**
terminfo capability, notcurses will employ it by default. This can be prevented
by setting **inhibit_alternate_screen** to **true**. Users tend to have strong
opinions regarding the alternate screen, so it's often useful to expose this
via a command-line option.

notcurses furthermore hides the cursor by default, but **retain_cursor** can
prevent this (the cursor can be dynamically enabled or disabled during
execution via **notcurses_cursor_enable(3)** and **notcurses_cursor_disable(3)**).

**notcurses_init** typically emits some diagnostics at startup, including version
information and some details of the configured terminal. This can be inhibited
with **suppress_banner**. This will also inhibit the performance summary normally
printed by **notcurses_stop(3)**.

Notcurses can render to a subregion of the terminal by specifying desired
margins on all four sides. By default, all margins are zero, and thus rendering
will be performed on the entirety of the viewing area. This is orthogonal to
use of the alternate screen; using the alternate screen plus margins will see
the full screen cleared, followed by rendering to a subregion. Inhibiting the
alternate screen plus margins will see rendering to a subregion, with the screen
outside this region not cleared. This is the only means by which existing
output can be undisturbed by notcurses. Margins are best-effort. Supplying any
negative margin is an error. **notcurses_lex_margins** provides lexing a
margin argument expression in one of two forms:

* a single number, which will be applied to all sides, or
* four comma-delimited numbers, applied to top, right, bottom, and left.

To allow future options without requiring redefinition of the structure, the
**flags** field is only a partially-defined bitfield. Undefined bits must be
zero. The following flags are defined:

* **NCOPTION_INHIBIT_SETLOCALE**: Unless this flag is set, **notcurses_init**
    will call **setlocale(LC_ALL, NULL)**. If the result is either "**C**" or
    "**POSIX**", it will print a diagnostic to **stder**, and then call
    **setlocale(LC_ALL, "").** This will attempt to set the locale based off
    the **LANG** environment variable. Your program should call **setlocale(3)**
    itself, usually as one of the first lines.


## Fatal signals

It is important to reset the terminal before exiting, whether terminating due
to intended operation or a received signal. This is usually accomplished by
explicitly calling **notcurses_stop(3)** during shutdown. For convenience, notcurses
by default installs signal handlers for various signals typically resulting in
process termination (see **signal(7)**). These signal handlers call
notcurses_stop(3) for each **struct notcurses** in the process, and then propagate
the signal to any previously-configured handler. These handlers are disabled
upon entry to **notcurses_stop(3)**.

To prevent signal handler registration, set **no_quit_sighandlers** to **true**.
No means is provided to selectively register fatal signal handlers. If this is
done, the caller ought be sure to effect similar functionality themselves.

## Resize events

**SIGWINCH** (SIGnal WINdow CHange) is delivered to the process when the terminal
is resized. The default action is to ignore it (**SIG_IGN**). notcurses installs
a handler for this signal. The handler causes notcurses to update its idea of
the terminal's size using **TIOCGWINSZ** (see **ioctl_tty(2)**), and generates an
**NCKEY_RESIZE** input event (see **notcurses_input(3)**. This signal handler can be
inhibited by setting **no_winch_sighandler** to **true**. If this is done, the
caller should probably watch for the signal, and invoke **notcurses_refresh(3)**
or **notcurses_render(3)** upon its receipt.

A resize event does not invalidate any references returned earlier by
notcurses. The content of any new screen area is undefined until the next call
to notcurses_render(3). This is true even if an existing **struct ncplane**
(see **notcurses_plane(3)**) overlaps the new area, since the signal could
arrive while the ncplanes are being modified. Signal handlers are quite
restricted as to what actions they can perform, so minimal work is performed in
the handler proper.

Thus, in the absence of **no_winch_sighandler**, **SIGWINCH** results in:

* interruption of some thread to process the signal
* a **TIOCGWINSZ** **ioctl** to retrieve the new screen size
* queuing of a **NCKEY_RESIZE** input event (if there is space in the queue)

Upon the next call to **notcurses_render(3)** or **notcurses_refresh(3)**, the
standard plane (see **notcurses_stdplane(3)**) will be resized to the new
screen size. The next **notcurses_render(3)** call will function as expected
across the new screen geometry.

# RETURN VALUES

**NULL** is returned on failure. Otherwise, the return value points at a valid
**struct notcurses**, which can be used until it is provided to
**notcurses_stop(3)**.

# SEE ALSO

**getenv(3)**,
**setlocale(3)**,
**termios(3)**,
**notcurses(3)**,
**notcurses_input(3)**,
**notcurses_plane(3)**,
**notcurses_refresh(3)**,
**notcurses_render(3)**,
**notcurses_stop(3)**,
**terminfo(5)**,
**signal(7)**
