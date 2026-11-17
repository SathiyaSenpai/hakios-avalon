# HakiOS

[English](README.md) · 中文 · [Українська](README.uk.md)

**运行在 OnePlus Nord 4（`avalon`，Qualcomm SM7675 Cliffs）上的 Apple 开源 Darwin 内核（XNU，xnu-8796.121.2）**，配备自有引导加载器、驱动、U-Boot 启动链和配置系统。这是一个研究平台，不是 ROM：默认从内存运行，重启后手机回到 Android。

**文档：** [docs/](docs/index.md)（英文）。

从 [Getting started](docs/getting-started/index.md)、[On the phone](docs/getting-started/phone.md) 或 [Port a device](docs/porting/index.md) 开始。OnePlus Nord 4（`avalon`）支持情况：[设备页面](docs/devices/avalon.md) 和 [文档首页](docs/index.md#what-works)。

## 仓库

| 仓库 | 内容 |
|---|---|
| `hakios-sm7675` | **OnePlus Nord 4（`avalon`）**：内核移植、驱动、引导加载器、用户空间、工具、文档和 `devices/avalon` |
| [`hakios-metal/`](hakios-metal) | 通过内核的 GPU 驱动，在 Adreno 上实现 Metal 的命令模型 |
| [hakios-uefi](https://github.com/oosexgang/hakios-uefi) | 启动引导加载器的固件（HakiOS 的 U-Boot） |
| [hackios-xnu-userland](https://github.com/oosexgang/hackios-xnu-userland) | 旧仓库：只为保留链接而存在的指向。用户空间现在在这里的 `userland/` |

它们如何组合：[仓库说明](docs/porting/repositories.md)。

## 你需要自带什么，以及这里永远没有什么

这些仓库中没有任何 Apple 的东西，也没有厂商固件。如果需要 Darwin 用户空间，你要用自己获取的 IPSW 自行布置；固件取自你自己的手机；没有任何东西会替你下载、解密或绕过。Apple 的服务（激活、Apple ID、iCloud、App Store）不受支持，也不是目标。[仓库里有什么，永远没有什么](docs/index.md#what-is-in-the-repository-and-what-never-is)。

## 参与贡献

见 [CONTRIBUTING.md](CONTRIBUTING.md) 和 [COMMUNITY.md](COMMUNITY.md)。

## 许可与法律

XNU 补丁及其派生文件采用 Apple Public Source License 2.0（[LICENSE.APSL](LICENSE.APSL)），其余采用 MIT（[LICENSE](LICENSE)），以文件头为准。第三方组件：[NOTICE](NOTICE)、[THIRD_PARTY_LICENSES.md](THIRD_PARTY_LICENSES.md)、[COMPLIANCE.md](COMPLIANCE.md)。分发什么、不分发什么：[LEGAL.md](LEGAL.md)。以上都不构成法律意见。

HakiOS 是一个独立的研究项目，**与 Apple 没有关联，也未获其认可**；提及 “Darwin”、“iPhone”、“XNU” 和 “Apple” 只是为了说明代码面向什么。如果你认为这里的某个文件侵犯了任何人的权利，请提交 issue，它会被移除。

## 团队

hakios 是一个实验性项目，致力于把 darwin 带到 android 硬件上。我们的目标是让 hakios 免费并人人可用。

@oodze — 创始人，@netusd — 推广 / ceo-dev，@wannqn — 开发，@hxfuxyyXD — 开发，Rhaenja Hun（杭润嘉）— 开发 / 赞助

感谢 silicium 团队提供的源代码！
