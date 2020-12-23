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
