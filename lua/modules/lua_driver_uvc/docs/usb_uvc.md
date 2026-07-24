# usb_uvc  —  require("usb_uvc")

- init();  wait_ready([ms=10000]) -> true|nil,err
- set_param({width=640, height=480, fps=15, format="mjpeg", buf_size=153600}) -> true|nil,err
  format: "mjpeg" (default) | "yuv" | "h264"
- stream_on() -> true|nil,err;  get_frame([timeout_ms=1000]) -> data|nil,err
- stream_off();  deinit()

Sequence: init -> wait_ready -> set_param -> stream_on -> get_frame* -> stream_off -> deinit.
Note: only one USB host module (usb_uvc or usb_msc) may be active at a time.
