# Tutorial 04:Spawning Threads


我们已经依据 audio 函数实现了音频支持,


```text
 ________ audio  _______      _____
|        | pkts |       |    |     | to spkr
| DECODE |----->| AUDIO |--->| SDL |-->
|________|      |_______|    |_____|
    |  video     _______
    |   pkts    |       |
    +---------->| VIDEO |
 ________       |_______|   _______
|       |          |       |       |
| EVENT |          +------>| VIDEO | to mon.
| LOOP  |----------------->| DISP. |-->
|_______|<---FF_REFRESH----|_______|
```