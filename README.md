# xclipp

CLI X11 clipboard owner.  
ICCCM compliant: supports `TIMESTAMP`, `TARGETS` and `MULTIPLE` targets.  
Supports large strings by implementing `INCR` mechanism.  
Supported textual formats:  

- `TEXT`
- `STRING`
- `UTF8_STRING`
- `C_STRING`

Supported file formats:

- `FILE_NAME`
- `text/uri-list`
- `x-special/gnome-copied-files`
- `x-special/KDE-copied-files`
- `x-special/mate-copied-files`
- `x-special/nautilus-clipboard`

### Usage

Copy `STRING` into clipboard, can then be retrieved with Ctrl+V or context menu paste:

```
xclipp [--] STRING
```

Copy file `FILE` into clipboard, can then be retrieved with Ctrl+V or context menu paste (wherever a file is expected, e.g. file managers, messengers that support sending files, etc.):

```
xclipp -f [--] FILE
```

Copy file's `FILE` content into clipboard, can then be retrieved with Ctrl+V or context menu paste:

```
xclipp -c [--] FILE
```

### Requirements

- C++20
- libxcb1-dev

### Build

```
mkdir build
cd build
cmake ..
make
```

