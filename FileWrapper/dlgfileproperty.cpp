#include "dlgfileproperty.h"
#include "archiveformat.h"
#include "filemetadata.h"

#include <QApplication>
#include <QClipboard>
#include <QDateTime>
#include <QFileInfo>
#include <QFontMetrics>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLayout>
#include <QLocale>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>

namespace {

QString archiveFormatText(quint32 nVersion)
{
    switch (nVersion)
    {
    case FwArc::VERSION_V1:
        return QCoreApplication::translate("DlgFileProperty", "FWDA v1（旧格式）");
    case FwArc::VERSION_V2:
        return QCoreApplication::translate("DlgFileProperty", "FWDA v2");
    case FwArc::VERSION_V3:
        return QCoreApplication::translate("DlgFileProperty", "FWDA v3");
    default:
        return QCoreApplication::translate("DlgFileProperty", "未知");
    }
}

// 文件时间统一按本地时区显示（与资源管理器一致）；0 表示归档未记录。
QString timestampText(qint64 nMs)
{
    if (!FwMeta::isPlausibleTimestamp(nMs))
        return QCoreApplication::translate("DlgFileProperty", "—");
    static const QLocale locale(QLocale::Chinese, QLocale::China);
    const QDateTime dt = QDateTime::fromMSecsSinceEpoch(nMs);
    return locale.toString(dt, QLocale::ShortFormat);
}

QString locationText(const DlgFileProperty::Data &data)
{
    if (!data.sLocation.isEmpty())
        return data.sLocation;
    // 文件没有目录段说明它就在归档根下；目录节点（公共根）本身没有上级可显示
    return data.bDir ? QCoreApplication::translate("DlgFileProperty", "—")
                     : QCoreApplication::translate("DlgFileProperty", "（归档根目录）");
}

QString crcText(const DlgFileProperty::Data &data)
{
    if (data.bHasCrc)
        return QStringLiteral("CRC32 ") + QString::asprintf("0x%08X", data.nCrc);
    if (data.bPending)
        return QCoreApplication::translate("DlgFileProperty", "不可用（新增条目尚未保存）");
    if (data.nArchiveVersion == FwArc::VERSION_V1)
        return QCoreApplication::translate("DlgFileProperty", "不可用（v1 归档未记录内容校验）");
    return QCoreApplication::translate("DlgFileProperty", "不可用");
}

// QLabel 的 WordWrap 只在"词边界"换行：没有空格/分隔符的超长 ASCII 段
// （例如 200 个 W 的文件名）会被整段裁掉，属性对话框里看不到完整内容。
// 这里给长串插入零宽断行点（U+200B），只影响显示：tooltip 与「复制信息」
// 仍然使用原始文本。分隔符本身已是断行点，不再重复插入。
QString wrapFriendlyText(const QString &sText)
{
    // 每 16 个字符给一个断点：14pt 标题与 10pt 正文在 520/385px 的可用宽度里
    // 都容得下 16 个西文字符，布局可以继续把多个块拼在一行，不会出现半个字被裁掉。
    static const int kMaxRun = 16;
    QString sOut;
    int nRun = 0;
    for (const QChar ch : sText)
    {
        sOut.append(ch);
        if (ch == QLatin1Char('/') || ch == QLatin1Char('\\') || ch == QLatin1Char('-')
            || ch == QLatin1Char('_') || ch == QLatin1Char('.'))
        {
            nRun = 0;
            continue;
        }
        if (++nRun >= kMaxRun)
        {
            sOut.append(QChar(0x200B));
            nRun = 0;
        }
    }
    return sOut;
}

// 标题最多占两行：更长的名字用中间省略号收紧，完整名字由 tooltip 与
// 「归档内路径」行（以及「复制信息」）保留。否则 200+ 字符的文件名会把
// 标题堆成十几行，属性窗口高到超出屏幕。
QString titleText(const QFont &baseFont, const QString &sName)
{
    static const int kTextWidth = 520;
    QFont fontTitle = baseFont;
    fontTitle.setPointSize(14);
    fontTitle.setBold(true);
    const QFontMetrics metrics(fontTitle);
    if (metrics.horizontalAdvance(sName) > kTextWidth * 2)
        return metrics.elidedText(sName, Qt::ElideMiddle, kTextWidth);
    return wrapFriendlyText(sName);
}

} // namespace

DlgFileProperty::DlgFileProperty(const Data &data, QWidget *parent)
    : QDialog(parent), m_data(data)
{
    setWindowTitle(tr("属性 - %1").arg(data.sName));
    setWindowFlag(Qt::WindowContextHelpButtonHint, false);
    // 固定宽度：信息是纵向列表，长路径换行显示，不靠把窗口撑宽来容纳
    setFixedWidth(560);

    auto *pRoot = new QVBoxLayout(this);
    pRoot->setContentsMargins(20, 16, 20, 14);
    pRoot->setSpacing(0);

    // 标题：文件名用大号粗体，下面一行灰字给类型与大小，一眼能看清"这是什么"。
    // 字号写在样式表里而不是 setFont()：light.qss 对 QWidget 统一设置了 font-size，
    // 用 id 选择器才能稳定盖过它。
    auto *pName = new QLabel(titleText(font(), data.sName), this);
    pName->setObjectName(QStringLiteral("propertyName"));
    pName->setTextFormat(Qt::PlainText);
    pName->setWordWrap(true);
    pName->setTextInteractionFlags(Qt::TextSelectableByMouse);
    pName->setToolTip(data.sName);
    pName->setStyleSheet(QStringLiteral(
        "QLabel#propertyName { font-size:14pt; font-weight:600; color:#111827; }"));
    pRoot->addWidget(pName);

    auto *pSubtitle = new QLabel(
        QStringLiteral("%1 · %2").arg(data.sType, FwArc::formatSize(data.nSize)), this);
    pSubtitle->setTextFormat(Qt::PlainText);
    pSubtitle->setStyleSheet(QStringLiteral("color:#6B7280;"));
    pRoot->addWidget(pSubtitle);
    pRoot->addSpacing(14);

    // 信息卡片：白底圆角。颜色与 light.qss 的主题色保持一致（边框 #E3E8EF / 分隔 #EDF0F5）
    auto *pCard = new QFrame(this);
    pCard->setObjectName(QStringLiteral("propertyCard"));
    pCard->setStyleSheet(QStringLiteral(
        "QFrame#propertyCard { background:#FFFFFF; border:1px solid #E3E8EF;"
        " border-radius:8px; }"));
    auto *pCardLay = new QVBoxLayout(pCard);
    pCardLay->setContentsMargins(18, 12, 18, 14);
    pCardLay->setSpacing(7);

    const QVector<Section> sections = buildSections(data);
    for (int i = 0; i < sections.size(); ++i)
    {
        if (i > 0)
        {
            auto *pLine = new QFrame(pCard);
            pLine->setFixedHeight(1);
            pLine->setStyleSheet(QStringLiteral("background:#EDF0F5; border:none;"));
            pCardLay->addSpacing(3);
            pCardLay->addWidget(pLine);
            pCardLay->addSpacing(3);
        }

        auto *pTitle = new QLabel(sections.at(i).sTitle, pCard);
        pTitle->setStyleSheet(QStringLiteral("color:#6B7280; font-weight:600;"));
        pCardLay->addWidget(pTitle);

        auto *pGrid = new QGridLayout;
        pGrid->setContentsMargins(0, 0, 0, 0);
        pGrid->setHorizontalSpacing(14);
        pGrid->setVerticalSpacing(7);
        pGrid->setColumnStretch(1, 1);

        int nRow = 0;
        for (const auto &row : sections.at(i).rows)
        {
            auto *pKey = new QLabel(row.first, pCard);
            pKey->setStyleSheet(QStringLiteral("color:#6B7280;"));
            pKey->setAlignment(Qt::AlignRight | Qt::AlignTop);
            pKey->setMinimumWidth(84);

            auto *pValue = new QLabel(wrapFriendlyText(row.second), pCard);
            pValue->setTextFormat(Qt::PlainText);
            pValue->setWordWrap(true);
            pValue->setTextInteractionFlags(Qt::TextSelectableByMouse);
            pValue->setToolTip(row.second);

            pGrid->addWidget(pKey, nRow, 0);
            pGrid->addWidget(pValue, nRow, 1);
            ++nRow;
        }
        pCardLay->addLayout(pGrid);
    }
    pRoot->addWidget(pCard);
    pRoot->addSpacing(16);

    auto *pCopy = new QPushButton(tr("复制信息"), this);
    pCopy->setObjectName(QStringLiteral("btnCopyInfo"));
    pCopy->setAutoDefault(false);
    pCopy->setMinimumWidth(104);
    connect(pCopy, &QPushButton::clicked, this, [this, pCopy]() {
        QApplication::clipboard()->setText(formatInfoText(m_data));
        pCopy->setText(tr("已复制"));
        // 定时器以按钮为上下文：对话框先关掉时不会回调到已析构的按钮
        QTimer::singleShot(1200, pCopy, [pCopy]() { pCopy->setText(DlgFileProperty::tr("复制信息")); });
    });

    auto *pClose = new QPushButton(tr("关闭"), this);
    pClose->setObjectName(QStringLiteral("btnCloseProperty"));
    pClose->setDefault(true);
    connect(pClose, &QPushButton::clicked, this, &QDialog::accept);

    auto *pButtons = new QHBoxLayout;
    pButtons->setContentsMargins(0, 0, 0, 0);
    pButtons->addWidget(pCopy);
    pButtons->addStretch(1);
    pButtons->addWidget(pClose);
    pRoot->addLayout(pButtons);

    // 按最终宽度算高度：QLabel 的 sizeHint 是按"自然宽度"给的，带换行的长路径
    // 会把它算高；直接用 sizeHint().height() 会在标题与内容之间留下一条空白。
    const int kWidth = 560;
    layout()->activate();
    resize(kWidth, layout()->totalHeightForWidth(kWidth));
}

QString DlgFileProperty::formatInfoText(const Data &data)
{
    QStringList lines;
    lines << tr("名称：%1").arg(data.sName);
    for (const Section &section : buildSections(data))
    {
        for (const auto &row : section.rows)
            lines << QStringLiteral("%1：%2").arg(row.first, row.second);
    }
    return lines.join(QLatin1Char('\n'));
}

QString DlgFileProperty::formatSize(qint64 nBytes)
{
    if (nBytes < 0)
        return QStringLiteral("—");
    static const QLocale locale(QLocale::Chinese, QLocale::China);
    const QString sExact = locale.toString(nBytes);
    if (nBytes < 1024)
        return tr("%1 字节").arg(sExact);
    return tr("%1（%2 字节）").arg(FwArc::formatSize(nBytes), sExact);
}

QString DlgFileProperty::typeTextForFile(const QString &sFileName)
{
    const QString sSuffix = QFileInfo(sFileName).suffix();
    return sSuffix.isEmpty() ? tr("文件") : tr("%1 文件").arg(sSuffix.toUpper());
}

QVector<DlgFileProperty::Section> DlgFileProperty::buildSections(const Data &data)
{
    QVector<Section> sections;

    Section basic;
    basic.sTitle = tr("基本信息");
    basic.rows.append(qMakePair(tr("类型"), data.sType));
    basic.rows.append(qMakePair(tr("位置"), locationText(data)));
    basic.rows.append(qMakePair(data.bDir ? tr("大小（合计）") : tr("大小"),
                                formatSize(data.nSize)));
    if (data.bDir)
    {
        basic.rows.append(qMakePair(tr("包含"),
            tr("%1 个文件、%2 个子文件夹").arg(data.nFileCount).arg(data.nDirCount)));
        basic.rows.append(qMakePair(tr("统计范围"),
            data.bFiltered ? tr("当前搜索筛选结果") : tr("当前列表中的归档条目")));
    }
    else
    {
        basic.rows.append(qMakePair(tr("压缩方式"), tr("不压缩（原样存储）")));
    }
    sections.append(basic);

    // 文件系统元数据（v3 起随归档保存；v1/v2 归档没有可显示的值）
    if (!data.bDir)
    {
        Section file;
        file.sTitle = tr("文件");
        if (data.bHasFsMetadata)
        {
            file.rows.append(qMakePair(tr("创建时间"), timestampText(data.nCreatedTime)));
            file.rows.append(qMakePair(tr("修改时间"), timestampText(data.nModifiedTime)));
            file.rows.append(qMakePair(tr("访问时间"), timestampText(data.nAccessedTime)));
            file.rows.append(qMakePair(tr("属性"), FwMeta::attributesText(data.nAttributes)));
        }
        else
        {
            file.rows.append(qMakePair(tr("文件时间"),
                tr("未记录（该归档版本未保存文件元数据）")));
        }
        sections.append(file);
    }

    Section archive;
    archive.sTitle = tr("归档信息");
    archive.rows.append(qMakePair(tr("所属归档"),
        data.sArchivePath.isEmpty() ? tr("—") : data.sArchivePath));
    archive.rows.append(qMakePair(tr("归档格式"), archiveFormatText(data.nArchiveVersion)));
    if (!data.sEntryPath.isEmpty())
        archive.rows.append(qMakePair(tr("归档内路径"), data.sEntryPath));
    if (!data.bDir)
    {
        archive.rows.append(qMakePair(tr("内容校验"), crcText(data)));
        archive.rows.append(qMakePair(tr("状态"),
            data.bPending ? tr("新增条目，尚未保存") : tr("已保存在归档中")));
    }
    sections.append(archive);

    return sections;
}
