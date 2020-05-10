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
当输出unpack end时脱壳完成, 20秒后将自动重新脱壳(已完全dump的dex将被忽略)
3. pull出dump文件
```bash
adb pull /data/data/cn.youlor.mydemo/unpacker
```
4. 调用修复工具
```bash
java dexfixer.jar -f /path/to/unpacker -o /path/to/output
```



## 编译

1. 下载android-7.1.2_r33完整源码
2. 替换unpacker/android-7.1.2_r33
3. 编译



### patch

查看unpacker/diff