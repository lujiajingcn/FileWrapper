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
    CONFIG(debug, debug|release) {
        DEPLOY_SUB_MAKEFILE = Makefile.Debug
    } else {
        DEPLOY_SUB_MAKEFILE = Makefile.Release
    }
    deploydeps.target   = deploydeps
    # 选当前配置的子 Makefile；直接 -C FileWrapper 会落到 wrapper 的固定 debug。
    deploydeps.commands = $(MAKE) -C FileWrapper -f $$DEPLOY_SUB_MAKEFILE deploydeps
    # 干净 qmake 后子目录 Makefile 还不存在：先让 qmake_all 生成再部署。
    deploydeps.depends  = sub-FileWrapper-qmake_all
    QMAKE_EXTRA_TARGETS += deploydeps
}
