# ZettaBridge — archived development status

**This fork is no longer being actively developed or maintained.** NetanelGuber
achieved the personal goal that motivated this fork and does not plan to continue
work on it. Issues, pull requests, compatibility fixes, and release updates may
not receive a response.

You are welcome to fork the repository and continue working on it yourself,
**subject to the existing [LICENSE](LICENSE)**. The license is source-available
and has noncommercial and perimeter restrictions; this notice does not change
those terms or grant additional rights. Please retain the license and credit
both [ZailoxTT](https://github.com/ZailoxTT), the original ZettaBridge creator,
and [NetanelGuber](https://github.com/NetanelGuber), the author of this
independent fork's work. This fork is not affiliated with or endorsed by the
original creator.

The previous project overview is preserved as [README_OLD.md](README_OLD.md).
The implementation plan and build instructions remain in [plan.md](plan.md) and
[docs/development.md](docs/development.md).

## Last recorded result

On a Pixel 11 Pro XL running Android 17, the fork converted the user's Google
Play copy of *Need for Speed: Most Wanted*, recognized its matching OBB, and
completed a local race with visible graphics, responsive controls, and sound.
One later locked-car browsing crash was recorded and did not reproduce in
follow-up runs. This is a limited device result, not a promise that other apps
will work. See [NFS_COMPATIBILITY.md](NFS_COMPATIBILITY.md) and the
[device evidence](docs/nfs-device-evidence.md) for the exact boundary.

## Last APK

The final release provides **ZettaBridge Manager**, the app for converting a
supported ARM32 APK and installing the converted package. It supports matching
ARM32 and x86 library folders by selecting the ARM32 libraries. The Manager
uses KernelSU only for a user-confirmed package installation or removal;
conversion runs without root. This is an experimental debug-signed build.
It does not include games, expansion files, signing keys, or a compatibility
guarantee. An app already converted with a different personal signing key
cannot be updated in place by the Manager unless that key is imported first.

The earlier plugin-style launcher is a separate app and is not part of this
release. Its documentation remains in [README_OLD.md](README_OLD.md).
