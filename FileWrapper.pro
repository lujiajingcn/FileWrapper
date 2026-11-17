TEMPLATE = subdirs

SUBDIRS += \
    FileWrapper \
    Plugins

# 在构建根目录也能补部署：mingw32-make deploydeps
#
# deploy.pri 里的同名目标定义在主程序子工程（FileWrapper/），而 subdirs 工程
# 不会自动转发自定义目标 —— 这里显式转发一层，避免用户为了「补复制一次运行库」
# 而必须 cd 进子目录。
win32 {
    deploydeps.target   = deploydeps
    deploydeps.commands = $(MAKE) -C FileWrapper deploydeps
    QMAKE_EXTRA_TARGETS += deploydeps
}
