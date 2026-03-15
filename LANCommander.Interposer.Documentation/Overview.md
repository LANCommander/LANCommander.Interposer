# Introduction
The LANCommander Interposer is a set of Windows API hooks designed to make games easier to run on modern platforms. The current version of the interposer adds the following functionality to almost any game:

- File path redirection
- Registry emulation
- FastDL support
- Borderless fullscreen windows

## Objective
While this project is under the LANCommander branding, its functionality and codebase live outside the LANCommander client/server. This compatibility shim was created solely to normalize the way games function within a Windows environment. With around 30 years of games released for post-DOS PCs and an endless amount of [standards](https://xkcd.com/927/) implemented by developers, Interposer was created specifically to tackle the following headaches:

- Games should not need administrator privileges to run
- Save paths should be able to be customized and coalesce into one location
- Any configuration of a game should happen within the game's directory or a user directory
- Games should not use the registry
- Games should be portable applications

This compatibility shim is _not_ meant to tackle things like graphics APIs or Linux compatibility. If you're looking for that, check out the following projects:

- [dgVoodoo2](https://dege.freeweb.hu/dgVoodoo2/) | Glide, DirectX1-7, Direct3D
- [DxWnd](https://dxwnd.com/) | Windowed mode, misc compatibilty fixes for _very_ old APIs
- [Proton](https://github.com/ValveSoftware/Proton) | Linux compatibilty for Steam games

## Installation and Use
Configuration and use of the Interposer is broken down on this site under the following resources:

- [Getting Started](GettingStarted)
- [Logging](Logging)
- [File Redirection](FileRedirection)
- [Registry Emulation](RegistryEmulation)
- [FastDL](FastDL)
- [Borderless Fullscreen](BorderlessFullscreen)