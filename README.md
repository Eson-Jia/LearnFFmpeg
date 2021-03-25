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