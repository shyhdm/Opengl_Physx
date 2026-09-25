# Opengl_Physx

Windows x64 OpenGL / NVIDIA PhysX 实验项目，包含刚体、软体、水体、沙子、Blast 和 Flow。

## 下载运行

1. 点击 **Code → Download ZIP**，完整解压仓库。
2. 双击根目录的 **启动.bat**。
3. 首次运行会从本仓库自动下载与源码版本一致的程序，之后可离线启动同一版本。

需要 Windows 10/11 x64、兼容项目所用 PhysX GPU / CUDA 的 NVIDIA 显卡及驱动。首次下载需要联网。不要在压缩包内直接双击启动文件。

源码推送后，自动构建需要时间。如果启动提示没有运行包，请到仓库的 **Actions → Windows runnable build** 查看是否完成。构建失败时不会用旧程序冒充新版。旧版下载目录不会自动变成新源码；获取新版仓库后再启动。

程序保存在 `Runtime/源码版本号`，每份源码使用自己的程序和设置。启动器不会要求安装 Visual Studio、CMake 或完整 CUDA Toolkit。

## 项目维护者的一次性配置

参见 [自动构建配置说明](自动构建配置说明.md)。以后推送 `main` 时自动编译和发布，不需要手动维护最新 Release。
