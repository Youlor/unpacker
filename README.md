# unpacker
基于ART的主动调用的脱壳机

## 使用方法
1. 配置待脱壳的app包名
```bash
adb shell
cd /data/local/tmp
echo cn.youlor.mydemo >> unpacker.config
```
2. 启动apk等待脱壳
  每隔10秒将自动重新脱壳(已完全dump的dex将被忽略)

  当输出unpack end时脱壳完成

3. pull出dump文件
```bash
adb pull /data/data/cn.youlor.mydemo/unpacker
```
4. 调用修复工具
```bash
java -jar dexfixer.jar /path/to/unpacker /path/to/output
```



## 编译

1. 下载android-7.1.2_r33完整源码
2. 替换unpacker/android-7.1.2_r33
3. 编译



## patch

查看unpacker/diff



## 常见问题

1. dump中途退出或卡死，重新启动进程，再次等待脱壳即可
2. 当前仅支持被壳保护的dex, 不支持App动态加载的dex/jar



## 原理



