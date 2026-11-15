#include "mainwindow.h"
#include "ui_mainwindow.h"

// Qt6 已移除 QTextCodec；本地编码回退改用 QString::fromLocal8Bit
#include <cstring>
#include <algorithm>
#include <limits>

MainWindow::MainWindow(QWidget *parent) :
    QMainWindow(parent),
    ui(new Ui::MainWindow)
{
    ui->setupUi(this);
}

MainWindow::~MainWindow()
{
    delete ui;
}

// 严格 UTF-8 合法性校验：遇到任何非法字节序列返回 false。
// 用于在“无 BOM”时区分 UTF-8 文件与 GBK/ANSI 等本地编码文件，
// 避免把 GBK 字节当成 UTF-8 解码成乱码。
static bool isStrictUtf8(const QByteArray &data)
{
    const char *p = data.constData();
    // QByteArray::size() 在 Qt6 返回 qsizetype（64 位）；用 int 保存会在
    // >2 GiB 时变负，循环体一次都不执行，把 GBK 大文件误判成合法 UTF-8。
    const qsizetype n = data.size();
    qsizetype i = 0;
    while (i < n)
    {
        const unsigned char c = (unsigned char)p[i];
        int extra; // 后续续字节个数
        if (c < 0x80)                 // ASCII
        {
            ++i;
            continue;
        }
        else if ((c & 0xE0) == 0xC0)  // 110xxxxx -> 2 字节
        {
            extra = 1;
            if (c < 0xC2) return false;        // 0xC0/0xC1 为过长短编码，非法
        }
        else if ((c & 0xF0) == 0xE0)  // 1110xxxx -> 3 字节
        {
            extra = 2;
        }
        else if ((c & 0xF8) == 0xF0)  // 11110xxx -> 4 字节
        {
            extra = 3;
            if (c > 0xF4) return false;        // > U+10FFFF，非法
        }
        else                          // 0x80-0xBF（续字节当首字节）或 0xF5-0xFF，均非法
        {
            return false;
        }
        if (i + extra >= n) return false;      // 字节数不足
        for (int k = 1; k <= extra; ++k)
        {
            if (((unsigned char)p[i + k] & 0xC0) != 0x80)
                return false;                  // 续字节必须是 10xxxxxx
        }
        i += extra + 1;
    }
    return true;
}

// 按编码解码文件内容为 QString：
//   1) 优先识别 BOM（UTF-8 / UTF-16LE / UTF-16BE）
//   2) 无 BOM 时，严格 UTF-8 校验通过则按 UTF-8 解析（覆盖绝大多数现代 .sql）
//   3) 否则按系统本地编码解析（中文 Windows 上为 GBK），避免 ANSI/GBK 文件乱码
QString MainWindow::decodeText(const QByteArray &data)
{
    // 1) BOM 检测
    if (data.size() >= 3
        && (unsigned char)data[0] == 0xEF
        && (unsigned char)data[1] == 0xBB
        && (unsigned char)data[2] == 0xBF)
    {
        return QString::fromUtf8(data.constData() + 3, data.size() - 3);
    }
    if (data.size() >= 2)
    {
        if ((unsigned char)data[0] == 0xFF && (unsigned char)data[1] == 0xFE) // UTF-16LE
            // Qt6 推荐 char16_t* 重载（ushort* 版本已废弃）；两者都是 16 位单元
            return QString::fromUtf16(reinterpret_cast<const char16_t *>(data.constData() + 2),
                                      (data.size() - 2) / 2);
        if ((unsigned char)data[0] == 0xFE && (unsigned char)data[1] == 0xFF) // UTF-16BE
        {
            QByteArray le = data.mid(2);
            unsigned char *b = reinterpret_cast<unsigned char *>(le.data());
            // le.size() 是 qsizetype（64 位）；用 int 计数在 >2 GiB 时会溢出为负，
            // 造成负下标越界写。
            for (qsizetype k = 0; k + 1 < le.size(); k += 2)
                std::swap(b[k], b[k + 1]);
            return QString::fromUtf16(reinterpret_cast<const char16_t *>(le.constData()), le.size() / 2);
        }
    }

    // 2) 无 BOM：严格 UTF-8 校验通过 -> UTF-8
    if (isStrictUtf8(data))
        return QString::fromUtf8(data);

    // 3) 回退到系统本地编码（中文 Windows 上为 GBK）
    // Qt6 中 codecForLocale()->toUnicode() 等价于 QString::fromLocal8Bit()
    return QString::fromLocal8Bit(data);
}

void MainWindow::clearContent()
{
    ui->textEdit->clear();
}

void MainWindow::showText(char *szBuf, qint64 nFileLen)
{
    if (szBuf == nullptr || nFileLen <= 0)
        return;

    // 使用 nFileLen 作为长度（而非 strlen），避免读取越界或 NUL 截断。
    // Qt6 的 QByteArray 长度类型为 qsizetype（64 位），无需强转 int。
    // 64 位下 qsizetype 与 qint64 同宽，原「超过 qsizetype::max()」判断恒为
    // false（死代码）；这里改成真实的可显示上限，防止超大文本冻结 UI 线程。
    constexpr qint64 kMaxDisplayBytes = 64 * 1024 * 1024;   // 64 MiB
    if (nFileLen > kMaxDisplayBytes)
    {
        ui->textEdit->setPlainText("文件过大，无法载入内存显示。");
        return;
    }
    QByteArray array(szBuf, static_cast<qsizetype>(nFileLen));
    // 必须用 setPlainText：setText 会把内容当富文本解析，打开 HTML/源码文件时
    // 标签会被吞掉，显示成渲染后的网页（本机 Qt 6.8.3 实测确认）。
    ui->textEdit->setPlainText(decodeText(array));
}
