# 自定义镜像的制作

## 1. 基础镜像的选择
我们需要选择一个基础镜像，在它的基础上进行修改。从项目 releases 页面下载最新的系统镜像文件（`.img` 格式）。
[镜像文件地址](https://github.com/chenlongos/AKA-00/releases/)

## 2. 镜像的挂载
我们需要挂载镜像文件到本地，以便进行修改。具体步骤如下：
```bash
sudo losetup -fP /path/to/image.img --show  # 把镜像对应到loop设备，返回loop设备的路径
# 假设返回的loop设备路径为/dev/loop0
sudo mount /dev/loop0p2 /mnt    # 挂载loop设备的p2分区到/mnt目录
```

## 3. 镜像的修改
在挂载后，我们可以对镜像文件进行修改。具体步骤如下：
```bash
cd /mnt/root/ # 进入挂载目录
rm -rf AKA-00 # 删除AKA-00目录
cp -r /path/to/AKA-00 . # 复制AKA-00目录到挂载目录
# 注：如果frontend有更新，需要重新生成static目录
```

# 4. 镜像的卸载
在修改完成后，我们可以卸载镜像文件。
```bash
sudo umount /mnt    # 卸载loop设备
sudo losetup -d /dev/loop0    # 卸载loop设备
```

# 5. 镜像的压缩
在修改完成后，我们可以压缩镜像文件，以便于传输。
```bash
xz -zk /path/to/image.img
```