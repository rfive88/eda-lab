# etc/

Copies/shims of scripts from OpenROAD's `etc/` directory so the embedded
submodule build (`add_subdirectory(third_party/openroad)`) can resolve
them by relative path — notably `find_messages.py`, which OpenROAD's CMake
runs to generate `messages.checked` for the `utl` message-id checks. The
other scripts (`cred_helper.py`, `whittle.py`, `file_to_string.py`, and
their tests) are part of the same upstream script set and are not used by
eda-lab directly.
