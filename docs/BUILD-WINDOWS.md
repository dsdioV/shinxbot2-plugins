# 在 Windows 上编译插件

上游的 `make_all.sh` 依赖 bash + make，官方 README 也写明「Windows 未验证」。
但在 Windows 上照样能编出可用的 `.so` —— 用 **WSL2** 即可。

本文记录实测通过的完整步骤（Ubuntu 24.04 + WSL2）。

## 为什么不能直接用 MinGW / MSYS2

不是编译器的问题，是**产物格式**的问题：插件必须是 **Linux ELF `.so`**，
由 Linux 版 `shinxbot` 主程序 `dlopen`。Windows 的 PE `.dll` 无法被加载。
所以交叉编译到 Linux 目标没有意义，要么在 WSL 里编，要么用 Docker。

**Docker 也可以**（框架自带的 `docker-compose.yml` 就是官方路径），
但它要多装一层 Docker Desktop；WSL 直编更轻。两者产物一致。

## 一次性准备

### 1. 确认 WSL 可用

```powershell
wsl -l -v
```

应能看到一个 Debian 系发行版（如 `Ubuntu-24.04`，STATE 为 Running/Stopped 均可）。
没有就 `wsl --install -d Ubuntu-24.04`。

### 2. 安装编译依赖

```bash
wsl -d Ubuntu-24.04 -u root -- bash -lc "
  apt-get update
  DEBIAN_FRONTEND=noninteractive apt-get install -y \
    build-essential cmake pkg-config \
    libmagick++-dev libfmt-dev libjsoncpp-dev libzip-dev"
```

### 3. 装 ImageMagick 7（关键坑）

**Ubuntu / Debian 的 apt 只提供 ImageMagick 6**，而框架要求 **7+**。
直接编会报：

```
error: 'class Magick::Image' has no member named 'alpha'
```

框架的 `Dockerfile` 正是因此从源码编译 IM7。同样做法：

```bash
wsl -d Ubuntu-24.04 -u root -- bash -lc '
  apt-get install -y curl xz-utils libjpeg-dev libpng-dev libtiff-dev \
    libgif-dev libwebp-dev libxml2-dev libltdl-dev
  cd /tmp
  curl -fsSLO https://github.com/ImageMagick/ImageMagick/archive/refs/tags/7.1.2-18.tar.gz
  tar -xzf 7.1.2-18.tar.gz && cd ImageMagick-7.1.2-18
  ./configure --prefix=/usr/local --with-modules --with-magick-plus-plus=yes --disable-static
  make -j"$(nproc)" && make install && ldconfig
  magick -version'
```

> 若 WSL 内 HTTPS 报证书错误，可在宿主机下载 tarball 再 `cp` 进去
> （实测就是这么绕过的）。

装完确认：

```bash
wsl -d Ubuntu-24.04 -u root -- bash -lc \
  'PKG_CONFIG_PATH=/usr/local/lib/pkgconfig pkg-config --modversion Magick++'
# 应输出 7.1.2（而不是 6.9.x）
```

## 编译插件

仓库里带了个包装脚本：

```bash
bash tools/build-plugin-wsl.sh nikohug
```

它会调用 WSL 里的 cmake 构建，产物落在 `./lib/functions/libnikohug.so`。
传 `all` 可以编全部插件。

想手动来也行：

```bash
wsl -d Ubuntu-24.04 -u root -- bash -lc '
  export PKG_CONFIG_PATH=/usr/local/lib/pkgconfig:$PKG_CONFIG_PATH
  cd /mnt/d/GitHub/shinxbot2-plugins/functions/nikohug
  mkdir -p build && cd build
  cmake -DCMAKE_BUILD_TYPE=Release .. && make -j$(nproc)'
```

## 验证产物

```bash
file lib/functions/libnikohug.so          # ELF 64-bit LSB shared object
nm -D --defined-only lib/functions/libnikohug.so | grep -E 'create_t|destroy_t'
```

两个工厂符号都在，就说明框架能 `dlopen` 它。

> 注意：`.so` 里 Magick++ 符号是**未定义**的（`U`），要靠主程序提供。
> 这是设计如此 —— 框架主程序自己链了 Magick++。所以单独 `dlopen`
> 这个 `.so` 会报 undefined symbol，属正常现象，不是打包错了。

## 把 .so 交给框架

框架和插件是两个仓库。编译产物要放到**框架仓库**：

```bash
cp lib/functions/libnikohug.so /d/GitHub/shinxbot2/lib/functions/
```

然后 `bot.load function nikohug`（或重启）即可加载。

## 已知限制

- 插件内部用 `file://` 绝对路径发图，这是 Linux 语义；
  若将来想直接跑 Windows 版框架，这层要改（上游本身也没做过）。
- WSL 通过 `/mnt/d` 访问 Windows 盘，构建时有 clock skew 警告，可忽略。
