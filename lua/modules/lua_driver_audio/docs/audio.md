# audio  —  require("audio")

Fixed-duration:
- h, err = new_input(sr, ch, 16 [, vol [, clk_pin, data_pin]])
- record_wav(h, path, ms)
- mic_read_level(h) -> rms
- close(h)

Streaming RX (DMIC):  start_record([sr[,ch[,clk,dat]]]) -> true;
  read_chunk([ms]) -> string|nil;  stop_record()
Streaming TX (speaker): start_play(sr, ch) -> true;
  write_chunk_play(data) -> true (blocks until previous chunk plays); stop_play()
