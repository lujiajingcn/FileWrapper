#include "mainwindow.h"
#include "ui_mainwindow.h"

#include <QTextCodec>
#include <cstring>
#include <algorithm>

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
    const int n = data.size();
    int i = 0;
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
            return QString::fromUtf16(reinterpret_cast<const ushort *>(data.constData() + 2),
                                      (data.size() - 2) / 2);
        if ((unsigned char)data[0] == 0xFE && (unsigned char)data[1] == 0xFF) // UTF-16BE
        {
            QByteArray le = data.mid(2);
            unsigned char *b = reinterpret_cast<unsigned char *>(le.data());
            for (int k = 0; k + 1 < le.size(); k += 2)
                std::swap(b[k], b[k + 1]);
            return QString::fromUtf16(reinterpret_cast<const ushort *>(le.constData()), le.size() / 2);
        }
    }

    // 2) 无 BOM：严格 UTF-8 校验通过 -> UTF-8
    if (isStrictUtf8(data))
        return QString::fromUtf8(data);

    // 3) 回退到系统本地编码（中文 Windows 上为 GBK），并兜底 toLocal8Bit
    QTextCodec *codec = QTextCodec::codecForLocale();
    if (codec)
        return codec->toUnicode(data);
    return QString::fromLocal8Bit(data);
}

void MainWindow::showText(char *szBuf, qint64 nFileLen)
{
    if (szBuf == nullptr || nFileLen <= 0)
        return;

    // 使用 nFileLen 作为长度（而非 strlen），避免读取越界或 NUL 截断
    QByteArray array(szBuf, (int)nFileLen);
    ui->textEdit->setText(decodeText(array));
}
