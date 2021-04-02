# LearnFFmpeg

FFmpeg 学习工程 

## env

| 环境   | 版本               | 描述          |
| -------- | ------------------ | ------ |
| ubuntu    | 18.04 LTS   |   |
| ffmpeg    | ffmpeg version n4.3.1-26-gca55240b8c | configuration: --enable-shared --enable-libx265 --enable-libx264 --enable-gpl --enable-libass2  |

### decode_video

该工程可以将`h264`裸流文件解码为一帧帧的图片

裸流可以通过`ffmpeg`生成处理编码格式为`h264`的文件产生
```bash
ffmpeg -i input.mp4 -vcodec copy output.h264
```
`decode_video output.h264 frame_name` 可以将文件解码成图片

### play_video

添加 filter 功能,从启动参数获取 filter description 并设置到播放器,运行命令格式为:

```bash
play_video input filter_description
```

filter description 举例:

- "split [main][tmp]; [tmp] crop=iw:ih/2:0:0, vflip [flip]; [main][flip] overlay=0:H/2"
    - 将图像在水平中间线翻转镜像
- "movie=/home/ubuntu/Pictures/6.png[logo];[in][logo]overlay=min(mod(-t*w*10\,W)\,W-w):min(H/W*mod(-t*w*10\,W)\,H-h)"

### remuxing

remuxing 可以支持读本地文件推 rtsp 流,需要注意需要修改一些地方:

1. 在调用`avformat_alloc_output_context2(&ofmt_ctx, NULL, "rtsp", out_filename)`需要传入`rtsp`格式
2. 在调用`avformat_write_header`的时候传入一下选项参数.
    1. 因为推流过去的服务器不支持 udp ,所以我们这里需要强制 tcp.

```c
    AVDictionay * dict = NULL;
    av_dict_set(&dict, "rtsp_transport", "tcp", 0);
    avformat_write_header(ofmt_ctx, &dict);
```


### 硬件加速编解码

参考:
- [ffmpeg实现硬件转码（使用FFmpeg调用NVIDIA GPU实现H265转码H264）](https://blog.csdn.net/qq_22633333/article/details/107701301)

### encode_video

### Syncing Video

教程中音视频同步是视频向音频同步,就是在音频的处理流程中获取音频的 pts *time_base.当视频的 schedule 到了之后,根据 对比两个 pts * time_base 之间的
差值调整对本帧的后续操作:加速或者慢速播放.