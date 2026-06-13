# usb_msc  —  require("usb_msc")

- init();  wait_ready([ms=10000]) -> true|nil,err;  mount() -> drive|nil,err
- umount();  deinit()
- write_file(path, data) -> true|nil,err;  read_file(path) -> data|nil,err
- list_dir([path]) -> entries;  remove(path) -> true|nil,err

Sequence: init -> wait_ready -> mount -> file ops -> umount -> deinit.
