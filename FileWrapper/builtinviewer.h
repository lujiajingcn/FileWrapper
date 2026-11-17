#ifndef BUILTINVIEWER_H
#define BUILTINVIEWER_H

#include "plugininterface.h"
#include <QString>

class QPlainTextEdit;
class QLabel;
class QWidget;

// 主程序内置的轻量查看器：为「没有插件处理的后缀」提供兜底入口（P2-2）。
// 实现 PluginInterface 以便直接复用 MainWindow 的多标签页 / TabDoc 机制。
class BuiltinViewer : public PluginInterface
{
public:
    enum Mode { HexMode, TextMode };
    explicit BuiltinViewer(Mode mode);
    ~BuiltinViewer() override;

    void sendFileData(char *szFileData, qint64 nFileLen) override;
    QWidget *getPluginWidget() override;
    void setSourceName(const QString &name) override;
    // 内置查看器与主程序同版本：直接跟随 PLUGIN_API_VERSION（B1）
    int apiVersion() const override { return PLUGIN_API_VERSION; }
    // 主程序直接 new 它（不经过 createInstance），但接口要求实现（B2）
    PluginInterface *createInstance() override { return new BuiltinViewer(m_mode); }

private:
    // C++17：static constexpr 成员隐含 inline，被 qMin 按引用使用（ODR-use）时
    // 无需在 .cpp 里再写一份类外定义；写成 static const 会让 GCC/MinGW 链接期报
    // undefined reference to `BuiltinViewer::kMaxDisplayBytes'（MSVC 宽松，不报）。
    static constexpr qint64 kMaxDisplayBytes = 1024 * 1024;   // 最多显示 1 MB，避免冻结
    Mode         m_mode;
    QWidget     *m_widget;
    QLabel      *m_title;
    QPlainTextEdit *m_edit;
    QString      m_sourceName;
};

#endif // BUILTINVIEWER_H
