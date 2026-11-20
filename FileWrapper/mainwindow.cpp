#include "mainwindow.h"
#include "icons.h"
#include "ui_mainwindow.h"
#include "formatresolver.h"
#include "filelistmodel.h"

#include <QLabel>
#include <QApplication>
#include <thread>
#include <QFileDialog>
#include "dlgmergefile.h"
#include "dlgsplitfile.h"
#include "dlgfileproperty.h"
#include "dlgentrytags.h"
#include "builtinviewer.h"
#include "pluginmanager.h"
#include "plugindiag.h"
#include <QDebug>
#include "dlgabout.h"
#include <QVariant>
#include "dlgoutputfile.h"
#include <QMessageBox>
#include <QPushButton>
#include <QAbstractButton>
#include <QDialog>
#include <QDialogButtonBox>
#include <QListWidget>
#include <QListWidgetItem>
#include <QPlainTextEdit>
#include <QProgressDialog>
#include <QProgressBar>
#include <QMenu>
#include <QToolButton>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QSet>
#include <functional>
#include <new>
#include <QList>
#include <QMap>
#include <QStyle>
#include <QIcon>
#include <QSignalBlocker>
#include <QDir>
#include <QFileInfo>
#include <QStatusBar>
#include <QDateTime>
#include <QSettings>
#include <QMouseEvent>
#include <QCloseEvent>
#include <QLineEdit>
#include <QTimer>
#include <QThread>
#include <QThreadPool>
#include <QRunnable>
#include <QEventLoop>
#include <QFile>
#include <QMutexLocker>
#include <QDeadlineTimer>
#include <QPainter>
#include <QStyleOptionTab>
#include <QTabWidget>
#include <QDockWidget>
#include <QKeySequence>
#include <QFontMetrics>
#include <QLayout>
#include <QResizeEvent>
#include <QFrame>
#include <QBuffer>
#include <QImage>
#include <QImageReader>
#include <QPainterPath>
#include <utility>          // std::move（C2：把读到的 QByteArray 移动进 PluginFile）
#include <cstring>          // std::memcmp（合并前探测已合并归档）

// 超过该阈值的文件优先走「临时文件 + 路径」通道，避免整块载入内存（P1-2）
static const qint64 kLargeFileThreshold = 128LL * 1024 * 1024;

// 文件列表「文件路径」列的最小宽度：面板再窄也保留这么宽，超出部分交给横向滚动条
static const int kMinPathColumnWidth = 140;

// 缩略图：网格里显示的图标是 96 逻辑像素。解码尺寸取 128：既比显示尺寸大一点、
// 缩小时仍然清晰，又不至于像 2x（192）那样每张固定 147 KiB。模型项会长期持有
// QIcon（缓存 clear() 并不会释放它们），所以这个像素尺寸直接决定大归档的内存
// 上限；128×128 ARGB32 约 64 KiB/张。
static const int kThumbnailIconSize  = 96;
static const int kThumbnailPixelSize = 128;
// 单张图片条目的预算：超过后不再整块读进内存解码（宁可显示类型角标也不卡死界面）
static const qint64 kThumbnailMaxBytes  = 32LL * 1024 * 1024;
static const qint64 kThumbnailMaxPixels = 40LL * 1000 * 1000;
// 每次重建视图最多生成多少张缩略图。项目图标由 QStandardItem 持有，
// 只清 QHash 缓存不会释放它们；必须有“本轮生成总数”上限，否则一万张图片
// 会线性吃过 GB 内存。超过上限后其余条目保留类型角标，用户可用筛选缩小范围。
static const int kThumbnailMaxGenerated = 1024;
static const int kThumbnailCacheLimit   = 1024;
// 后台同时读取/解码的张数。固定为 1：单张 40 MP 图仍可能先解出约 160 MiB
// 再缩放，两个并发就会把峰值翻倍；缩略图本来只是预览，没必要用内存换吞吐。
// 保存前也最多只需等这一个任务，等待时间可控。
static const int kThumbnailMaxInFlight  = 1;

// 列表项是否为“文件叶子节点”：只有文件节点携带 FILEINFO，目录节点不带数据
static bool isFileLeafItem(const QStandardItem *pItem)
{
    return pItem != nullptr && pItem->data(FileListModel::InfoRole).isValid();
}

// 「标记」列显示文本：多个标记用顿号连接；空串 = 没有标记。
static QString tagsColumnText(const FILEINFO &info)
{
    return info.listTags.join(QStringLiteral("、"));
}

// 路径归一化：统一分隔符为 '/'，去掉结尾多余的 '/'（保留盘符根如 "E:/"）
static QString normalizePath(const QString &sPath)
{
    QString s = sPath;
    s.replace('\\', '/');
    while (s.endsWith('/') && s.size() > 1 && s.at(s.size() - 2) != ':')
        s.chop(1);
    return s;
}

// 计算所有条目路径的最长公共“目录”前缀（最后一段是文件名，不参与比较）
// 例如 E:/d/a/x.txt 与 E:/d/b/y.txt -> ["E:", "d"]；跨盘符时返回空
static QStringList commonDirPrefix(const QVector<FILEINFO> &vtInfos)
{
    QStringList common;
    bool bFirst = true;
    for (const FILEINFO &fi : vtInfos)
    {
        QStringList segs = normalizePath(fi.sFilePath).split('/', Qt::SkipEmptyParts);
        int nDirCount = qMax(0, segs.size() - 1);   // 排除文件名段

        if (bFirst)
        {
            for (int i = 0; i < nDirCount; ++i)
                common << segs.at(i);
            bFirst = false;
        }
        else
        {
            int nKeep = 0;
            const int nMax = qMin(common.size(), nDirCount);
            while (nKeep < nMax && common.at(nKeep) == segs.at(nKeep))
                ++nKeep;
            while (common.size() > nKeep)
                common.removeLast();
        }

        if (common.isEmpty())
            break;   // 已无公共前缀（如跨盘符），无需继续比较
    }
    return common;
}

// 单链目录压缩：某目录节点只有唯一一个子目录时，把父子并成一行（a -> b -> c 显示为 "a/b/c"）
static void compactSingleChainDirs(QStandardItem *pDir)
{
    if (pDir == nullptr)
        return;

    // 先自底向上处理已有子树，保证合并进来的子节点也已压缩过
    for (int i = 0; i < pDir->rowCount(); ++i)
    {
        QStandardItem *pChild = pDir->child(i);
        if (pChild != nullptr && !isFileLeafItem(pChild))
            compactSingleChainDirs(pChild);
    }

    while (pDir->rowCount() == 1)
    {
        QStandardItem *pOnly = pDir->child(0);
        if (pOnly == nullptr || pOnly->rowCount() == 0 || isFileLeafItem(pOnly))
            break;   // 文件/空目录：链条到底（空目录必须 break，否则内层 while 不收敛会死循环）

        pDir->setText(pDir->text() + "/" + pOnly->text());
        pDir->setToolTip(pDir->text());

        // 把孙节点整体上移为该节点的子节点
        while (pOnly->rowCount() > 0)
        {
            QList<QStandardItem*> kids = pOnly->takeRow(0);
            pDir->appendRow(kids);
        }
        pDir->removeRow(0);   // 删掉已空的子目录壳，避免残留幽灵行
    }
}

// ============================================================================
// FwFlowLayout / FwActionBar：会换行的图标工具条
// 背景：QToolBar 在窄栏里会把放不下的动作折进溢出菜单（"»"），按钮直接消失；
// 左侧文件列表面板宽度变化很大，改成流式布局后 8 个按钮始终可见。
// ============================================================================

FwFlowLayout::FwFlowLayout(QWidget *parent, int nMargin, int nHSpacing, int nVSpacing)
    : QLayout(parent), m_nHSpace(nHSpacing), m_nVSpace(nVSpacing)
{
    setContentsMargins(nMargin, nMargin, nMargin, nMargin);
}

FwFlowLayout::~FwFlowLayout()
{
    QLayoutItem *pItem = nullptr;
    while ((pItem = takeAt(0)) != nullptr)
        delete pItem;
}

void FwFlowLayout::addItem(QLayoutItem *item)
{
    m_items.append(item);
}

int FwFlowLayout::count() const
{
    return m_items.size();
}

QLayoutItem *FwFlowLayout::itemAt(int index) const
{
    return m_items.value(index);
}

QLayoutItem *FwFlowLayout::takeAt(int index)
{
    if (index >= 0 && index < m_items.size())
        return m_items.takeAt(index);
    return nullptr;
}

Qt::Orientations FwFlowLayout::expandingDirections() const
{
    return {};
}

bool FwFlowLayout::hasHeightForWidth() const
{
    return true;
}

int FwFlowLayout::heightForWidth(int nWidth) const
{
    return doLayout(QRect(0, 0, nWidth, 0), true);
}

void FwFlowLayout::setGeometry(const QRect &rect)
{
    QLayout::setGeometry(rect);
    doLayout(rect, false);
}

QSize FwFlowLayout::sizeHint() const
{
    return minimumSize();
}

QSize FwFlowLayout::minimumSize() const
{
    QSize size;
    for (const QLayoutItem *pItem : m_items)
        size = size.expandedTo(pItem->minimumSize());
    const QMargins m = contentsMargins();
    return size + QSize(m.left() + m.right(), m.top() + m.bottom());
}

// 按可用宽度排布：放不下就换行；返回本宽度下需要的总高度（bTestOnly 时只算不摆）
int FwFlowLayout::doLayout(const QRect &rect, bool bTestOnly) const
{
    const QMargins m = contentsMargins();
    const QRect rcArea = rect.adjusted(m.left(), m.top(), -m.right(), -m.bottom());
    int x = rcArea.x();
    int y = rcArea.y();
    int nLineHeight = 0;

    for (QLayoutItem *pItem : m_items)
    {
        const QSize sz = pItem->sizeHint();
        const bool bWrap = (x + sz.width() > rcArea.right() + 1) && (nLineHeight > 0);
        if (bWrap)
        {
            x = rcArea.x();
            y += nLineHeight + m_nVSpace;
            nLineHeight = 0;
        }
        if (!bTestOnly)
            pItem->setGeometry(QRect(QPoint(x, y), sz));
        x += sz.width() + m_nHSpace;
        nLineHeight = qMax(nLineHeight, sz.height());
    }
    return y + nLineHeight - rect.y() + m.bottom();
}

FwActionBar::FwActionBar(QWidget *parent) : QWidget(parent)
{
    setObjectName(QStringLiteral("fileActionBar"));
    setAttribute(Qt::WA_StyledBackground, true);

    m_pLayout = new FwFlowLayout(this, 0, 2, 2);
    m_pLayout->setContentsMargins(6, 4, 6, 4);

    QSizePolicy policy(QSizePolicy::Preferred, QSizePolicy::Minimum);
    policy.setHeightForWidth(true);
    setSizePolicy(policy);
}

void FwActionBar::setIconSize(const QSize &size)
{
    m_iconSize = size;
    for (QToolButton *pBtn : m_buttons)
        pBtn->setIconSize(size);
}

void FwActionBar::addAction(QAction *pAction)
{
    if (pAction == nullptr)
        return;
    auto *pBtn = new QToolButton(this);
    pBtn->setDefaultAction(pAction);          // 使能/勾选状态由 QAction 统一驱动
    pBtn->setToolButtonStyle(Qt::ToolButtonIconOnly);
    pBtn->setAutoRaise(true);
    pBtn->setIconSize(m_iconSize);
    pBtn->setFixedSize(QSize(30, 30));
    pBtn->setFocusPolicy(Qt::NoFocus);
    pBtn->setCursor(Qt::PointingHandCursor);
    m_buttons.append(pBtn);
    m_pLayout->addWidget(pBtn);
    updateGeometry();
}

void FwActionBar::addSeparator()
{
    auto *pSep = new QWidget(this);
    pSep->setObjectName(QStringLiteral("actionSep"));
    pSep->setFixedSize(QSize(9, 30));
    auto *pLine = new QFrame(pSep);
    pLine->setObjectName(QStringLiteral("actionSepLine"));
    pLine->setGeometry(4, 7, 1, 16);
    pLine->setFrameShape(QFrame::NoFrame);
    m_pLayout->addWidget(pSep);
    updateGeometry();
}

// 兼容原 QToolBar 调用点：本控件固定图标模式、不可拖动
void FwActionBar::setToolButtonStyle(Qt::ToolButtonStyle)
{
}

void FwActionBar::setMovable(bool)
{
}

void FwActionBar::setFloatable(bool)
{
}

int FwActionBar::preferredHeightForWidth(int nWidth) const
{
    return m_pLayout != nullptr ? m_pLayout->heightForWidth(nWidth) : 38;
}

QSize FwActionBar::sizeHint() const
{
    return QSize(240, preferredHeightForWidth(240));
}

QSize FwActionBar::minimumSizeHint() const
{
    return QSize(30, 30);
}

// 父布局若不按 heightForWidth 分配，就在宽度变化后自己把高度调到需要值，
// 保证换行后的第二行不会被裁掉。相等时不重设，避免布局循环。
void FwActionBar::resizeEvent(QResizeEvent *pEvent)
{
    QWidget::resizeEvent(pEvent);
    if (m_bInHeightFix)
        return;
    const int nNeed = preferredHeightForWidth(width());
    if (nNeed > 0 && height() != nNeed)
    {
        m_bInHeightFix = true;
        setFixedHeight(nNeed);
        m_bInHeightFix = false;
    }
}

// ============================================================================
// 标签页「新建 +」按钮：贴在最后一个标签右侧（替代 setCornerWidget 甩到窗口最右）
// ============================================================================
void MainWindow::updateNewTabButton()
{
    if (m_btnNewTab == nullptr)
        return;
    QTabBar *pBar = ui->tabWidget->tabBar();
    if (pBar == nullptr)
        return;

    if (pBar->count() <= 0)
    {
        m_btnNewTab->hide();
        return;
    }

    const QRect rcLast = pBar->tabRect(pBar->count() - 1);
    if (!rcLast.isValid() || rcLast.isEmpty())
    {
        m_btnNewTab->hide();
        return;
    }

    const int nSize = qMin(24, qMax(18, rcLast.height() - 6));
    // QTabBar 自身宽度只等于所有标签的宽度之和，右侧空白属于 QTabWidget，
    // 所以「+」挂在 QTabWidget 上，用 mapTo 换算坐标。
    const QPoint ptLast = pBar->mapTo(ui->tabWidget, rcLast.topLeft());
    const int nX = ptLast.x() + rcLast.width() + 6;
    const int nY = ptLast.y() + (rcLast.height() - nSize) / 2;
    if (nX + nSize > ui->tabWidget->width() - 2)
    {
        // 标签已经占满整行：藏起来，避免压住标签文字
        m_btnNewTab->hide();
        return;
    }
    const QRect rcTarget(nX, nY, nSize, nSize);
    if (m_btnNewTab->geometry() != rcTarget)
        m_btnNewTab->setGeometry(rcTarget);
    m_btnNewTab->show();
    m_btnNewTab->raise();
}

// 运行期绘制的标签页类型角标：品牌色圆角方块 + 一个字母。
// 不依赖额外的图片资源，任何 DPI 下都清晰。
// 尺寸同时覆盖 16–32px 的标签/列表小图标与 96–128px 的缩略图占位图标。
static QIcon makeTypeBadge(const QString &sLetter, const QColor &clrBadge)
{
    QIcon icon;
    for (int nSize : {16, 24, 32, 48, 64, 96, 128})
    {
        QPixmap pm(nSize, nSize);
        pm.fill(Qt::transparent);

        QPainter painter(&pm);
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setPen(Qt::NoPen);
        painter.setBrush(clrBadge);
        const qreal dRadius = nSize * 0.28;
        painter.drawRoundedRect(QRectF(0.5, 0.5, nSize - 1.0, nSize - 1.0), dRadius, dRadius);

        QFont font(QStringLiteral("Segoe UI"));
        font.setBold(true);
        font.setPixelSize(qMax(8, int(nSize * 0.62)));
        painter.setFont(font);
        painter.setPen(QColor(QStringLiteral("#FFFFFF")));
        painter.drawText(QRect(0, 0, nSize, nSize), Qt::AlignCenter, sLetter);
        painter.end();

        icon.addPixmap(pm);
    }
    return icon;
}

// 停靠面板标题栏「浮动 / 关闭」按钮的图标：中性灰线性图形，运行期绘制。
// 不用系统自带的 SP_TitleBarNormalButton / SP_TitleBarCloseButton——Fusion 给的
// 那两枚图标自身带一圈立体边框，压进 16px 就成了一团灰色小方块，和本主题的扁平
// 风格也打架。X 的取色与 resources/theme/icons/close_light.xpm 保持一致。
static QIcon makeDockTitleIcon(bool bClose)
{
    QIcon icon;
    for (int nSize : {16, 24, 32, 48})
    {
        QPixmap pm(nSize, nSize);
        pm.fill(Qt::transparent);

        QPainter painter(&pm);
        painter.setRenderHint(QPainter::Antialiasing, true);
        QPen pen(QColor(QStringLiteral("#8A93A2")));
        pen.setWidthF(qMax(1.0, nSize / 11.0));
        pen.setCapStyle(Qt::RoundCap);
        pen.setJoinStyle(Qt::RoundJoin);
        painter.setPen(pen);
        painter.setBrush(Qt::NoBrush);

        if (bClose)
        {
            const qreal dInset = nSize * 0.22;
            painter.drawLine(QPointF(dInset, dInset), QPointF(nSize - dInset, nSize - dInset));
            painter.drawLine(QPointF(nSize - dInset, dInset), QPointF(dInset, nSize - dInset));
        }
        else
        {
            // 两个错位的圆角方框 = 「浮出成独立窗口」（VS Code / Office 同款语义）。
            // 交叠处先用 Clear 抠掉再画前框，两条描边不会糊成一团。
            const qreal dUnit = nSize * 0.14;
            const qreal dRadius = dUnit * 0.5;
            const QRectF rcBack(dUnit, dUnit, nSize - 3 * dUnit, nSize - 3 * dUnit);
            const QRectF rcFront(2 * dUnit, 2 * dUnit, nSize - 3 * dUnit, nSize - 3 * dUnit);
            painter.drawRoundedRect(rcBack, dRadius, dRadius);

            const qreal dGap = pen.widthF() + 0.5;
            painter.save();
            painter.setCompositionMode(QPainter::CompositionMode_Clear);
            painter.setPen(Qt::NoPen);
            painter.setBrush(Qt::black);
            painter.drawRoundedRect(rcFront.adjusted(-dGap, -dGap, dGap, dGap), dRadius, dRadius);
            painter.restore();

            painter.setPen(pen);
            painter.drawRoundedRect(rcFront, dRadius, dRadius);
        }
        painter.end();

        icon.addPixmap(pm);
    }
    return icon;
}

// 按后缀把文件归到 9 类，返回对应角标（未知类型给灰色问号）
// 标签页/列表的类型角标：类别来自插件元数据 JSON 的 "category"（image/media/document…），
// 主程序只维护"类别 -> 字母/颜色"这一张表。
// 以前这里是按后缀硬编码的 9 组规则（90 多个后缀），插件新增格式时角标会掉成 "?"；
// 现在某个后缀归哪个类别由插件自己声明，新增后缀/新插件不需要改这里。
QIcon MainWindow::tabIconForFile(const QString &sFileName)
{
    struct BadgeStyle
    {
        const char *szLetter;
        const char *szColor;
    };
    static const QHash<QString, BadgeStyle> kStyles = {
        { QStringLiteral("document"), { "W", "#2B6CB0" } },
        { QStringLiteral("sheet"),    { "X", "#1F8A54" } },
        { QStringLiteral("slides"),   { "S", "#D97706" } },
        { QStringLiteral("pdf"),      { "P", "#E5484D" } },
        { QStringLiteral("image"),    { "I", "#7C3AED" } },
        { QStringLiteral("media"),    { "M", "#0E9AA7" } },
        { QStringLiteral("book"),     { "B", "#B45309" } },
        { QStringLiteral("text"),     { "T", "#4B5563" } },
        { QStringLiteral("archive"),  { "Z", "#6D28D9" } },
    };

    const QString sExt = QFileInfo(sFileName).suffix().toLower();
    QString sCategory = PluginManager::getInstance()->categoryForExtension(sExt);
    if (sCategory.isEmpty())
    {
        // 没有插件认领的压缩包仍给 Z 角标（只是展示类型，不代表本程序能打开）
        const QString sId = FormatResolver::byExtension(sExt).formatId;
        if (sId == QStringLiteral("zip") || sId == QStringLiteral("rar")
            || sId == QStringLiteral("7z") || sId == QStringLiteral("tar")
            || sId == QStringLiteral("gz"))
            sCategory = QStringLiteral("archive");
    }

    static QHash<QString, QIcon> s_mapIcons;   // 类别 -> 已生成的角标（按类别缓存）
    const auto styleIt = kStyles.constFind(sCategory);
    if (styleIt == kStyles.constEnd())
    {
        static const QIcon s_iconUnknown =
                makeTypeBadge(QStringLiteral("?"), QColor(QStringLiteral("#94A3B8")));
        return s_iconUnknown;
    }

    auto it = s_mapIcons.constFind(sCategory);
    if (it == s_mapIcons.constEnd())
        it = s_mapIcons.insert(sCategory,
                               makeTypeBadge(QString::fromLatin1(styleIt->szLetter),
                                             QColor(QString::fromLatin1(styleIt->szColor))));
    return it.value();
}

// 「缩略图显示」工具按钮图标：2×2 圆角块（左上角用品牌青色强调），
// 与 view_tree / view_flat 一样是运行期绘制的深蓝线性图标，不新增资源文件。
static QIcon makeThumbnailViewIcon()
{
    QIcon icon;
    for (int nSize : {20, 24, 32, 40, 48, 64})
    {
        QPixmap pm(nSize, nSize);
        pm.fill(Qt::transparent);

        QPainter painter(&pm);
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setPen(Qt::NoPen);

        const qreal dGap = nSize * 0.10;
        const qreal dCell = (nSize - 3.0 * dGap) / 2.0;
        const QColor clrDark(QStringLiteral("#1F2A3D"));
        const QColor clrCyan(QStringLiteral("#0FB2FB"));
        for (int r = 0; r < 2; ++r)
        {
            for (int c = 0; c < 2; ++c)
            {
                const QRectF rcCell(dGap + c * (dCell + dGap),
                                    dGap + r * (dCell + dGap), dCell, dCell);
                painter.setBrush((r == 0 && c == 0) ? clrCyan : clrDark);
                const qreal dRadius = qMax<qreal>(1.0, nSize * 0.07);
                painter.drawRoundedRect(rcCell, dRadius, dRadius);
            }
        }
        painter.end();
        icon.addPixmap(pm);
    }
    return icon;
}

// 窗口标题：归档名 + 未保存标记（和标签页的关闭确认逻辑保持一致）
void MainWindow::updateWindowTitle()
{
    const QString sArchive = m_fileManager->archivePath();
    if (!m_fileManager->isArchiveLoaded() || sArchive.isEmpty())
    {
        setWindowTitle(tr("文件查看器"));
        return;
    }
    setWindowTitle(tr("文件查看器 - %1%2")
                       .arg(QFileInfo(sArchive).fileName(),
                            m_fileManager->needsSave() ? QStringLiteral(" *") : QString()));
}

MainWindow::MainWindow(QWidget *parent) :
    QMainWindow(parent),
    ui(new Ui::MainWindow)
{
    ui->setupUi(this);

    // 缩略图解码离开 GUI 线程：专职线程池，单并发（另见 kThumbnailMaxInFlight）。
    m_thumbPool = new QThreadPool(this);
    m_thumbPool->setMaxThreadCount(kThumbnailMaxInFlight);

    // 停播超时后的非阻塞复查：不再次阻塞等插件，只轮询 isPlaybackStopped()。
    m_stopUnsafePoll = new QTimer(this);
    m_stopUnsafePoll->setInterval(1000);
    connect(m_stopUnsafePoll, &QTimer::timeout,
            this, &MainWindow::pollStopUnsafeInstances);

    // 文件管理器需先于视图初始化：视图初始化时会查询“归档是否已加载”来设置工具栏可用状态
    m_fileManager = new FileManager;

    initWidgetFilePath();
    initWidgetFileContent();
    restoreUiState();
    purgeLegacyPluginMapSettings();

    // 所有停靠窗创建完之后统一处理标题栏按钮（新增停靠窗时在这里能一并覆盖）
    polishDockTitleButtons();

    // 启动时一次性加载所有插件（避免每次双击重复 reload）
    PluginManager::getInstance()->loadAllPlugins();

    // 收集所有插件的 widget 指针，关闭/卸载标签页时据此避免误删插件自身持有的 widget
    for (PluginInterface* iface : PluginManager::getInstance()->getAllInterfaces())
        m_setPluginWidgets.insert(iface->getPluginWidget());

    connect(this, &MainWindow::QMergeFiles, m_fileManager, &FileManager::QMergeFiles);
    connect(this, &MainWindow::QSplitFiles, m_fileManager, &FileManager::QSplitFiles);
    connect(m_fileManager, &FileManager::warningOccurred,
            this, &MainWindow::onFileManagerWarning);

    // 常用快捷键直接挂在动作本身上：动作被 setEnabled(false) 后快捷键也失效，
    // 避免“按钮灰了但 Ctrl+S/Ctrl+U 仍能触发”的不一致。
    ui->actionLoadFile->setShortcut(QKeySequence::Open);
    ui->actionMergeFile->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_M));
    m_acSaveFile->setShortcut(QKeySequence::Save);
    m_acOutputFile->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_E));
    ui->actionUnloadFile->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_U));

    // 注意：不要给菜单栏上的 QMenu 设图标。QMenuBar 计算菜单项宽度时只按文字算、
    // 绘制时却把图标画在文字位置，结果「插件设置」会只剩一个图标、文字被挤没
    // （2026-11-19 实测，150% DPI 同样复现）。菜单项（QAction）的图标不受影响。

    // 「校验归档」加入文件菜单（不改 .ui，避免影响已有布局）
    m_acVerify = new QAction(QStringLiteral("校验归档内容"), ui->menu);
    m_acVerify->setToolTip(QStringLiteral("逐条目检查内容区 CRC32，检测归档是否被篡改或损坏"));
    m_acVerify->setIcon(FwIcons::get(QStringLiteral("diagnose")));
    connect(m_acVerify, &QAction::triggered, this, &MainWindow::onActionVerifyArchiveTriggered);
    ui->menu->addAction(m_acVerify);

    // 插件设置（合并入口）：插件状态/依赖、支持的格式、启用禁用、重新加载都在一个
    // 对话框里。以前是四个菜单项，信息互相重叠，启停后还要再点一次「重新加载」。
    QAction *actPluginSettings = new QAction(QStringLiteral("插件设置…"), ui->menu_2);
    actPluginSettings->setToolTip(QStringLiteral("插件状态与依赖、支持的格式、启用/禁用、重新加载"));
    actPluginSettings->setIcon(FwIcons::get(QStringLiteral("manage")));
    connect(actPluginSettings, &QAction::triggered, this,
            &MainWindow::onActionPluginSettingsTriggered);
    ui->menu_2->addAction(actPluginSettings);

    // 所有动作创建完毕后统一进入初始状态（启动时未加载归档）
    syncActionStates();

    // 启动时若已有插件加载失败，给一条用户可见的提示（以前只进 qDebug，双击文件毫无反应）
    const int nFailedAtStartup = PluginManager::getInstance()->failures().size();
    if (nFailedAtStartup > 0)
        ui->statusBar->showMessage(
            QStringLiteral("有 %1 个插件加载失败，见「插件设置」查看原因")
                .arg(nFailedAtStartup), 20000);

    // 清理上一次会话遗留的临时抽取文件。只删 24 小时以前的陈旧文件，
    // 避免误删同一台机器上另一个正在运行的实例刚抽取出来的大文件。
    QDir tmpDir(QDir::tempPath() + QStringLiteral("/FileWrapper"));
    if (tmpDir.exists())
    {
        const QDateTime cutoff = QDateTime::currentDateTime().addDays(-1);
        const QStringList old = tmpDir.entryList(QStringList() << QStringLiteral("fw_*"), QDir::Files);
        for (const QString &n : old)
        {
            const QFileInfo fi(tmpDir.absoluteFilePath(n));
            if (!fi.lastModified().isValid() || fi.lastModified() < cutoff)
                tmpDir.remove(n);
        }
    }
}

MainWindow::~MainWindow()
{
    if (m_stopUnsafePoll != nullptr)
        m_stopUnsafePoll->stop();

    // 缩略图任务也会打开归档文件；先作废代数并等它们读完，再销毁窗口/线程池。
    // QPointer 在任务回调里校验窗口是否还活着，等待失败时任务也不会访问已销毁的 this。
    cancelThumbnailGeneration();
    waitForThumbnailReads(3000);

    // 后台 CRC 线程可能仍在读归档并通过 qApp 投递回调：析构前先请求中止并等它退出，
    // 避免析构期间回调再去访问窗口状态。
    waitForMappedCrc(3000);

    // 析构期间不再触发 activateTab，避免销毁标签页时重入渲染逻辑
    disconnect(ui->tabWidget, nullptr, this, nullptr);

    // P0-3：先尝试停止**所有**标签页的实例（不仅是当前页）；停不下来的不能让
    // 它们的映射/实例/DLL 进入释放路径。
    ensureAllInstancesStopped();
    for (int i = ui->tabWidget->count() - 1; i >= 0; --i)
        destroyTabAt(i);

    if (!m_setStopUnsafe.isEmpty() || !m_vtDeferredInstances.isEmpty())
    {
        // 进程即将退出：保留映射、实例和 loader，禁止 PluginManager 析构时 unload DLL
        // （后台线程若仍在执行插件代码，unload 会立刻崩溃）。
        PluginManager::getInstance()->setUnloadEnabled(false);
        qWarning() << "[Lifecycle] 退出时仍有停播未确认的插件实例，保留映射并跳过 DLL 卸载";
    }

    releaseAllMappedInputs();   // 兜底：只释放已确认安全的映射
    cleanupTempFiles();

    if (m_tvFilePath)
    {
        m_tvFilePath->viewport()->removeEventFilter(this);
        // 排序恢复的过滤器装在表头 viewport 上，析构时一并移除
        if (QHeaderView *pHeader = m_tvFilePath->header())
            pHeader->viewport()->removeEventFilter(this);
    }
    if (m_lvFilePath)
        m_lvFilePath->viewport()->removeEventFilter(this);
    delete m_fileManager;
    delete m_hModelFilePath;
    m_hModelFilePath = nullptr;
    delete ui;
}

void MainWindow::cleanupTempFiles()
{
    discardTempCache();
}

// 切换归档时丢弃本会话抽取出的临时文件：此时后台播放已停止、插件实例已销毁，
// 文件不再被占用。避免旧归档的临时副本一直占着磁盘。
void MainWindow::discardTempCache()
{
    // P0-3/P1-8：停播未确认的实例仍可能按路径读取临时文件。
    // 这些文件必须保留到进程退出，删除会让后台线程读到已删除/被替换的内容
    // （Windows 上通常删不掉，但共享模式下也可能成功）。
    if (!m_setStopUnsafe.isEmpty() || !m_vtDeferredInstances.isEmpty())
    {
        m_mapTempCache.clear();
        m_listTempOrder.clear();
        qWarning() << "[TempCache] 有停播未确认的插件实例，保留临时副本不删除";
        return;
    }
    for (const QString &p : m_setTempFiles)
        QFile::remove(p);
    m_setTempFiles.clear();
    m_mapTempCache.clear();
    m_listTempOrder.clear();
}

void MainWindow::onFileManagerWarning(const QString &sMsg)
{
    statusBar()->showMessage(sMsg, 10000);
}

// 插件拒绝打开时的说明（接口 v6）：插件给了真实原因就按原因说。
// 以前不管什么原因，界面上一律是「插件无法显示该文件」—— 于是
// "这台机器没有音频输出设备"被用户当成"文件坏了"，按提示永远修不好。
QString MainWindow::pluginFailureText(const QString &sFileName, const QString &sWhy)
{
    const QString sReason = sWhy.trimmed();
    if (sReason.isEmpty())
        return tr("插件无法显示该文件：%1").arg(sFileName);
    return tr("无法打开 %1：%2").arg(sFileName, sReason);
}

// 初始化【文件列表】窗口
void MainWindow::initWidgetFilePath()
{
    m_hModelFilePath = new FileListModel;
    m_hModelFilePath->setColumnCount(4);
    m_hModelFilePath->setHeaderData(0, Qt::Horizontal, tr("文件路径"));
    m_hModelFilePath->setHeaderData(1, Qt::Horizontal, tr("大小"));
    m_hModelFilePath->setHeaderData(2, Qt::Horizontal, tr("类型"));
    m_hModelFilePath->setHeaderData(3, Qt::Horizontal, tr("标记"));
    m_tvFilePath = ui->treeView;
    m_tvFilePath->setModel(m_hModelFilePath);
    // 列宽策略：
    //   0 文件路径 —— 自己管理（Interactive），宽度 = 视口宽 - 其余三列，且不低于
    //               kMinPathColumnWidth；这样面板拖窄时路径列不会被挤成一条缝，
    //               放不下就出横向滚动条。
    //   1 大小 / 2 类型 —— 按内容自适应。
    //   3 标记 —— 固定初始宽度、允许手动调整：标记长度差异很大，按内容自适应会
    //             被某个超长标记把路径列挤没。
    QHeaderView *pHeader = m_tvFilePath->header();
    pHeader->setStretchLastSection(false);
    pHeader->setSectionResizeMode(0, QHeaderView::Interactive);
    pHeader->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    pHeader->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    pHeader->setSectionResizeMode(3, QHeaderView::Interactive);
    pHeader->resizeSection(3, 110);
    connect(pHeader, &QHeaderView::geometriesChanged, this, [this]() {
        if (m_bAdjustingPathColumn)
            return;
        // 延后一拍：让 ResizeToContents 的两列先算出新宽度
        QTimer::singleShot(0, this, [this]() { adjustPathColumnWidth(); });
    });
    m_tvFilePath->setSortingEnabled(true);   // 平铺/树形都允许列排序；树形为同级排序
    // 列表观感：统一行高、斑马纹、收敛缩进（缩进太大会把深层文件名挤没）。
    // 注意 uniformRowHeights 必须在填充模型之前设置，否则视图可能不刷新行。
    m_tvFilePath->setUniformRowHeights(true);
    m_tvFilePath->setAlternatingRowColors(true);
    m_tvFilePath->setIndentation(14);
    m_tvFilePath->setAllColumnsShowFocus(true);
    // 表头的鼠标事件是发到 header 的 viewport 上的，过滤器必须装在 viewport 上，
    // 否则收不到按下列头的事件，排序后无法恢复展开状态。
    m_tvFilePath->header()->viewport()->installEventFilter(this);

    // 缩略图视图：与树形共用同一个模型（只显示第 0 列），单独放在 QStackedWidget
    // 里随模式切换。列表只做“展示成什么样”，文件数据/右键/删除/导出仍走同一套业务。
    m_lvFilePath = new QListView(ui->dockWidgetContents);
    m_lvFilePath->setObjectName(QStringLiteral("thumbnailView"));
    m_lvFilePath->setAccessibleName(tr("文件缩略图"));
    m_lvFilePath->setModel(m_hModelFilePath);
    m_lvFilePath->setModelColumn(0);
    m_lvFilePath->setViewMode(QListView::IconMode);
    m_lvFilePath->setFlow(QListView::LeftToRight);
    m_lvFilePath->setWrapping(true);
    m_lvFilePath->setResizeMode(QListView::Adjust);
    m_lvFilePath->setMovement(QListView::Static);
    m_lvFilePath->setIconSize(QSize(kThumbnailIconSize, kThumbnailIconSize));
    // 单元格要同时容纳图标和最多两行文件名；宽度再留出一点左右留白。
    m_lvFilePath->setGridSize(QSize(kThumbnailIconSize + 36, kThumbnailIconSize + 50));
    m_lvFilePath->setSpacing(6);
    m_lvFilePath->setWordWrap(true);
    m_lvFilePath->setUniformItemSizes(true);
    m_lvFilePath->setTextElideMode(Qt::ElideRight);
    m_lvFilePath->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_lvFilePath->setSelectionMode(QAbstractItemView::SingleSelection);
    m_lvFilePath->setSelectionBehavior(QAbstractItemView::SelectItems);

    m_swFilePathViews = new QStackedWidget(ui->dockWidgetContents);
    m_swFilePathViews->setObjectName(QStringLiteral("fileViewStack"));
    m_swFilePathViews->addWidget(m_tvFilePath);
    m_swFilePathViews->addWidget(m_lvFilePath);
    m_swFilePathViews->setCurrentWidget(m_tvFilePath);

    // 文件列表工具条：图标化 + 按语义分组。
    // 用 FwActionBar（流式布局）而不是 QToolBar：QToolBar 在窄栏里会把放不下的动作
    // 折进溢出菜单（"»"）导致按钮直接消失；FwActionBar 改成排满换行，8 个按钮始终可见。
    m_tbFilePath = new FwActionBar(ui->dockWidgetContents);
    m_tbFilePath->setIconSize(QSize(18, 18));

    m_acViewTree = new QAction("树形显示", m_tbFilePath);
    m_acViewFlat = new QAction("平铺显示", m_tbFilePath);
    m_acViewThumbnail = new QAction("缩略图显示", m_tbFilePath);
    m_acShowFilePath = new QAction("显示路径", m_tbFilePath);
    m_acShowFileName = new QAction("显示名称", m_tbFilePath);
    m_acAddFile = new QAction("添加文件", m_tbFilePath);
    m_acDelFile = new QAction("删除文件", m_tbFilePath);
    m_acOutputFile = new QAction("导出文件", m_tbFilePath);
    m_acSaveFile = new QAction("保存文件", m_tbFilePath);
    m_acSaveFile->setEnabled(false);

    m_acViewTree->setToolTip(tr("树形显示：按合并前的目录层级展示"));
    m_acViewFlat->setToolTip(tr("平铺显示：不显示层级，每个文件一行"));
    m_acViewThumbnail->setToolTip(tr("缩略图显示：网格展示文件，图片条目显示内容预览"));
    m_acShowFilePath->setToolTip(tr("平铺/缩略图模式下显示文件的完整路径"));
    m_acShowFileName->setToolTip(tr("平铺/缩略图模式下仅显示文件名"));
    m_acAddFile->setToolTip(tr("添加文件到当前归档"));
    m_acDelFile->setToolTip(tr("从归档中删除选中的文件"));
    m_acOutputFile->setToolTip(tr("把选中的文件导出到磁盘"));
    m_acSaveFile->setToolTip(tr("保存对归档的改动（Ctrl+S）"));

    // 视图（树/平铺/缩略图）｜显示（路径/名称）｜文件（增删导出保存）
    m_tbFilePath->addAction(m_acViewTree);
    m_tbFilePath->addAction(m_acViewFlat);
    m_tbFilePath->addAction(m_acViewThumbnail);
    m_tbFilePath->addSeparator();
    m_tbFilePath->addAction(m_acShowFilePath);
    m_tbFilePath->addAction(m_acShowFileName);
    m_tbFilePath->addSeparator();
    m_tbFilePath->addAction(m_acAddFile);
    m_tbFilePath->addAction(m_acDelFile);
    m_tbFilePath->addAction(m_acOutputFile);
    m_tbFilePath->addAction(m_acSaveFile);

    // 文件列表 dock 可关闭且 saveState() 会持久化隐藏态；必须提供恢复入口，
    // 否则用户关掉后再启动就再也看不到列表（只能删 ini）。
    // 同理给主工具栏一个 windowTitle：QMainWindow 的右键菜单会把它列成开关项，
    // 而 QToolBar 的开关项文字取自 windowTitle，.ui 里没设就会变成一个只有勾选、
    // 没有文字的空白项。
    ui->mainToolBar->setWindowTitle(tr("主工具栏"));
    QMenu *pViewMenu = menuBar()->addMenu(tr("视图"));
    pViewMenu->addAction(ui->mainToolBar->toggleViewAction());
    pViewMenu->addAction(ui->dwFilePath->toggleViewAction());

    // 筛选框：单独占一行并拉满宽度（按路径/文件名/标记过滤，三种视图都生效）
    QWidget *pSearchRow = new QWidget(ui->dockWidgetContents);
    pSearchRow->setObjectName(QStringLiteral("searchRow"));
    QHBoxLayout *pSearchLayout = new QHBoxLayout(pSearchRow);
    pSearchLayout->setContentsMargins(8, 8, 8, 3);
    pSearchLayout->setSpacing(0);

    m_leFilter = new QLineEdit(pSearchRow);
    m_leFilter->setObjectName(QStringLiteral("leFilter"));
    m_leFilter->setPlaceholderText(tr("搜索文件名、路径或标记…"));
    m_leFilter->setClearButtonEnabled(true);
    m_leFilter->setMinimumWidth(160);
    pSearchLayout->addWidget(m_leFilter);
    // P1-9：每次按键都全量重建树/列表，在大归档下会明显卡顿。
    // 去抖 150 ms：连续输入只刷新最后一次；回车/清空仍然立刻生效。
    m_filterDebounce = new QTimer(this);
    m_filterDebounce->setSingleShot(true);
    m_filterDebounce->setInterval(150);
    connect(m_filterDebounce, &QTimer::timeout, this, [this]() {
        m_sFilter = m_leFilter->text().trimmed();
        refreshFilePathView();
    });
    connect(m_leFilter, &QLineEdit::textChanged, this, [this](const QString &sText) {
        Q_UNUSED(sText);
        m_filterDebounce->start();
    });
    connect(m_leFilter, &QLineEdit::returnPressed, this, [this]() {
        if (m_filterDebounce) m_filterDebounce->stop();
        m_sFilter = m_leFilter->text().trimmed();
        refreshFilePathView();
    });

    connect(m_acViewTree, &QAction::triggered, this, &MainWindow::onActionViewTreeTriggered);
    connect(m_acViewFlat, &QAction::triggered, this, &MainWindow::onActionViewFlatTriggered);
    connect(m_acViewThumbnail, &QAction::triggered, this,
            &MainWindow::onActionViewThumbnailTriggered);
    connect(m_acShowFilePath, &QAction::triggered, this, &MainWindow::onActionShowFilePathTriggered);
    connect(m_acShowFileName, &QAction::triggered, this, &MainWindow::onActionShowFileNameTriggered);
    connect(m_acAddFile, &QAction::triggered, this, &MainWindow::onActionAddFileTriggered);
    connect(m_acDelFile, &QAction::triggered, this, &MainWindow::onActionDelFileTriggered);
    connect(m_acOutputFile, &QAction::triggered, this, &MainWindow::onActionOutputFileTriggered);
    connect(m_acSaveFile, &QAction::triggered, this, &MainWindow::onActionSaveFileTriggered);
    // 删除/导出依赖“当前是否选中具体文件”，选中变化后立即刷新可用状态
    connect(m_tvFilePath->selectionModel(), &QItemSelectionModel::currentChanged,
            this, [this](const QModelIndex &, const QModelIndex &) { syncActionStates(); });
    connect(m_tvFilePath->selectionModel(), &QItemSelectionModel::selectionChanged,
            this, [this](const QItemSelection &, const QItemSelection &) { syncActionStates(); });
    connect(m_lvFilePath->selectionModel(), &QItemSelectionModel::currentChanged,
            this, [this](const QModelIndex &, const QModelIndex &) { syncActionStates(); });
    connect(m_lvFilePath->selectionModel(), &QItemSelectionModel::selectionChanged,
            this, [this](const QItemSelection &, const QItemSelection &) { syncActionStates(); });

    // 图标：统一使用 resources/icons（应用图标同款蓝青风格）
    setWindowIcon(FwIcons::get(QStringLiteral("appicon")));
    ui->actionLoadFile->setIcon(FwIcons::get(QStringLiteral("load")));
    ui->actionUnloadFile->setIcon(FwIcons::get(QStringLiteral("unload")));
    ui->actionMergeFile->setIcon(FwIcons::get(QStringLiteral("merge")));
    ui->actionSplitFile->setIcon(FwIcons::get(QStringLiteral("split")));
    ui->actionAbout->setIcon(FwIcons::get(QStringLiteral("about")));
    m_acViewTree->setIcon(FwIcons::get(QStringLiteral("view_tree")));
    m_acViewFlat->setIcon(FwIcons::get(QStringLiteral("view_flat")));
    m_acViewThumbnail->setIcon(makeThumbnailViewIcon());
    m_acShowFilePath->setIcon(FwIcons::get(QStringLiteral("show_path")));
    m_acShowFileName->setIcon(FwIcons::get(QStringLiteral("show_name")));
    m_acAddFile->setIcon(FwIcons::get(QStringLiteral("add")));
    m_acDelFile->setIcon(FwIcons::get(QStringLiteral("delete")));
    m_acOutputFile->setIcon(FwIcons::get(QStringLiteral("export")));
    m_acSaveFile->setIcon(FwIcons::get(QStringLiteral("save")));

    // 主工具栏：只保留最常用文件操作，且**只显示图标**——文字放在 tooltip 里，
    // 鼠标悬停时才出现。菜单「文件处理」里仍是「图标 + 文字」，可发现性不受影响；
    // tooltip 顺带标出快捷键，省得再去菜单里找。
    ui->mainToolBar->setToolButtonStyle(Qt::ToolButtonIconOnly);
    ui->mainToolBar->setIconSize(QSize(24, 24));
    ui->actionLoadFile->setToolTip(tr("加载文件（Ctrl+O）"));
    ui->actionUnloadFile->setToolTip(tr("卸载文件（Ctrl+U）"));
    ui->actionMergeFile->setToolTip(tr("合并文件（Ctrl+M）"));
    ui->actionSplitFile->setToolTip(tr("分割文件"));
    ui->mainToolBar->addAction(ui->actionLoadFile);
    ui->mainToolBar->addAction(ui->actionUnloadFile);
    ui->mainToolBar->addSeparator();
    ui->mainToolBar->addAction(ui->actionMergeFile);
    ui->mainToolBar->addAction(ui->actionSplitFile);

    // 左侧面板三段式：搜索行 / 图标工具条 / 文件视图（树形/平铺 与 缩略图共用一个容器）
    QVBoxLayout *vbLytFilePath = new QVBoxLayout(ui->dockWidgetContents);
    vbLytFilePath->setContentsMargins(0, 0, 0, 0);
    vbLytFilePath->setSpacing(0);
    vbLytFilePath->addWidget(pSearchRow);
    vbLytFilePath->addWidget(m_tbFilePath);
    vbLytFilePath->addWidget(m_swFilePathViews, 1);

    // 默认树形模式，故平铺/缩略图模式专属的按钮置灰；同时统一刷新保存按钮等状态
    syncActionStates();

    // 文件列表右键菜单（含“打开新标签页”）：树形与缩略图各自弹同一份菜单
    m_tvFilePath->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_tvFilePath, &QTreeView::customContextMenuRequested,
            this, &MainWindow::onTreeViewCustomContextMenu);
    m_lvFilePath->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_lvFilePath, &QListView::customContextMenuRequested,
            this, &MainWindow::onThumbnailViewCustomContextMenu);

    // 关闭 QTreeView 自带的「双击展开/折叠」，改由 eventFilter 统一处理：
    // 自带行为会在业务侧 setExpanded() 之后再次翻转节点，两者互相抵消。
    m_tvFilePath->setExpandsOnDoubleClick(false);
    m_tvFilePath->viewport()->installEventFilter(this);
    m_lvFilePath->viewport()->installEventFilter(this);
}

// 停靠面板标题栏按钮（浮动 / 关闭）的尺寸与图标。
// 实测（Qt 6.9.3 + Fusion + light.qss）：Fusion 自带的图标带立体边框，主题又给这
// 两颗按钮 2px 内边距，结果按钮只有 18×10、图标被压到 6px 高——既看不清也点不准，
// 高缩放下更糊。这里统一放大到 22×22（16px 图标 + 3px 留白），与主工具栏 24px、
// 文件列表工具条 18px 的图标尺度对齐，并换成上面的线性图标。
// 尺寸用 setFixedSize 而不是 QSS：QDockWidgetTitleButton::sizeHint() 已经把 style
// 的 padding 算进去了，再在 QSS 里写 min-width 会和主题里已有的 padding 互相打架。
void MainWindow::polishDockTitleButtons()
{
    const QSize szButton(22, 22);
    const QSize szIconSize(16, 16);

    auto polishOne = [&](QDockWidget *pDock, const char *szObjectName, bool bClose,
                         const QString &sTip, const QString &sAccessibleName) {
        auto *pBtn = pDock->findChild<QAbstractButton *>(QString::fromLatin1(szObjectName));
        // 对象名是 Qt 的内部约定（QDockWidgetPrivate 里写死）；版本不同查不到时静默
        // 跳过，退化成系统默认按钮，功能不受影响。
        if (!pBtn)
            return;
        pBtn->setFixedSize(szButton);
        pBtn->setIconSize(szIconSize);
        pBtn->setIcon(makeDockTitleIcon(bClose));
        pBtn->setToolTip(sTip);
        pBtn->setAccessibleName(sAccessibleName);
        pBtn->setCursor(Qt::PointingHandCursor);
    };

    for (QDockWidget *pDock : findChildren<QDockWidget *>())
    {
        polishOne(pDock, "qt_dockwidget_floatbutton", false,
                  tr("浮动为独立窗口"), tr("浮动面板"));
        polishOne(pDock, "qt_dockwidget_closebutton", true,
                  tr("关闭面板（可从「视图」菜单重新打开）"), tr("关闭面板"));
    }
}

// 初始化【文件内容】窗口（多标签页）
void MainWindow::initWidgetFileContent()
{
    // 清空 UI 默认自带的占位标签页
    ui->tabWidget->clear();
    ui->tabWidget->setTabsClosable(true);   // 允许关闭标签页
    ui->tabWidget->setMovable(true);        // 允许拖动标签排序

    // 「新建标签页 +」贴在最后一个标签右侧（以前用 setCornerWidget 放在窗口最右边，
    // 标签在左、按钮在右，鼠标要走大半个屏幕）。这里用叠在标签栏上的普通按钮实现，
    // 标签增删/窗口缩放后由 updateNewTabButton() 重新定位。
    m_btnNewTab = new QToolButton(ui->tabWidget);
    m_btnNewTab->setObjectName(QStringLiteral("btnNewTab"));
    m_btnNewTab->setText(QStringLiteral("+"));
    m_btnNewTab->setToolTip(tr("新建标签页（Ctrl+T）"));
    m_btnNewTab->setAutoRaise(true);
    m_btnNewTab->setCursor(Qt::PointingHandCursor);
    m_btnNewTab->setFixedSize(24, 24);
    connect(m_btnNewTab, &QToolButton::clicked, this, &MainWindow::onActionNewTabTriggered);

    // 标签栏事件：中键关闭 + 布局变化后重排「+」
    ui->tabWidget->tabBar()->installEventFilter(this);
    ui->tabWidget->installEventFilter(this);
    ui->tabWidget->tabBar()->setElideMode(Qt::ElideMiddle);
    ui->tabWidget->tabBar()->setUsesScrollButtons(true);

    connect(ui->tabWidget, &QTabWidget::tabCloseRequested,
            this, &MainWindow::onTabCloseRequested);
    connect(ui->tabWidget, &QTabWidget::currentChanged,
            this, &MainWindow::onTabCurrentChanged);
    // P0-2：允许拖动标签，但必须同步 m_vTabDocs，否则后续按标签下标取文档
    // 会取到另一个实例（关闭/停播/删除都可能作用到错误对象）。
    connect(ui->tabWidget->tabBar(), &QTabBar::tabMoved,
            this, &MainWindow::onTabBarTabMoved);

    // 标签页相关快捷键（挂在窗口上，窗口激活即可用）
    auto addWindowShortcut = [this](const QKeySequence &seq, const std::function<void()> &fn) {
        QAction *pAction = new QAction(this);
        pAction->setShortcut(seq);
        pAction->setShortcutContext(Qt::WindowShortcut);
        connect(pAction, &QAction::triggered, this, fn);
        addAction(pAction);
    };
    addWindowShortcut(QKeySequence(Qt::CTRL | Qt::Key_W), [this]() {
        const int nIndex = ui->tabWidget->currentIndex();
        if (nIndex >= 0)
            onTabCloseRequested(nIndex);
    });
    addWindowShortcut(QKeySequence(Qt::CTRL | Qt::Key_T), [this]() {
        onActionNewTabTriggered();
    });
    addWindowShortcut(QKeySequence(Qt::CTRL | Qt::Key_Tab), [this]() {
        const int nCount = ui->tabWidget->count();
        if (nCount > 1)
            ui->tabWidget->setCurrentIndex((ui->tabWidget->currentIndex() + 1) % nCount);
    });
    addWindowShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_Tab), [this]() {
        const int nCount = ui->tabWidget->count();
        if (nCount > 1)
            ui->tabWidget->setCurrentIndex((ui->tabWidget->currentIndex() - 1 + nCount) % nCount);
    });
    addWindowShortcut(QKeySequence(Qt::CTRL | Qt::Key_F), [this]() {
        if (m_leFilter != nullptr)
        {
            m_leFilter->setFocus(Qt::ShortcutFocusReason);
            m_leFilter->selectAll();
        }
    });

    // 默认新建一个空白标签页
    newEmptyTab();
    updateNewTabButton();
}

// 新建一个空白标签页（无文件）
void MainWindow::newEmptyTab()
{
    TabDoc doc;
    doc.hasFile = false;
    m_vTabDocs.append(doc);

    QWidget *placeholder = new QWidget();
    QVBoxLayout *ly = new QVBoxLayout(placeholder);
    QLabel *lbl = new QLabel(placeholder);
    lbl->setText("空白标签页\n\n在左侧文件列表中双击文件，或右键选择“打开新标签页”以在此查看文件内容。");
    lbl->setAlignment(Qt::AlignCenter);
    lbl->setWordWrap(true);
    ly->addWidget(lbl);

    int newIdx = ui->tabWidget->addTab(placeholder, "新标签页");
    ui->tabWidget->setCurrentIndex(newIdx); // 触发 currentChanged -> activateTab
}

// 关闭全部标签页（用于加载/卸载归档时重置）
void MainWindow::closeAllTabs()
{
    // 断开信号，避免逐个 removeTab 触发多余的 activateTab
    disconnect(ui->tabWidget, &QTabWidget::currentChanged, this, &MainWindow::onTabCurrentChanged);
    disconnect(ui->tabWidget, &QTabWidget::tabCloseRequested, this, &MainWindow::onTabCloseRequested);

    // P0-3：逐个停止**所有**实例（以前只停当前实例）。停播失败的实例会转入
    // 延期清理：保留映射/实例/loader，不再走 delete 与 DLL 卸载。
    if (!ensureAllInstancesStopped())
    {
        PluginManager::getInstance()->setUnloadEnabled(false);
        qWarning() << "[Lifecycle] 关闭全部标签页时有实例停播未确认，禁止卸载插件 DLL";
    }

    // 逐个销毁标签页（含各自独占的插件实例，会连带销毁其内部窗口）
    for (int i = ui->tabWidget->count() - 1; i >= 0; --i)
        destroyTabAt(i);

    // 旧归档的临时副本已无人使用，立即回收磁盘（避免反复加载大归档堆积 GB 级副本）
    discardTempCache();
    releaseAllMappedInputs();

    // 归档/标签页重置：CRC 缓存以展示路径为键，跨归档会碰撞（同名但内容不同的
    // 大文件会被误判为已校验）；推进代数让仍在飞的后台校验结果不再写回。
    ++m_nArchiveEpoch;
    m_setMappedCrcPending.clear();
    m_setMappedCrcVerified.clear();

    m_pCurrentInterface = nullptr;

    connect(ui->tabWidget, &QTabWidget::currentChanged, this, &MainWindow::onTabCurrentChanged);
    connect(ui->tabWidget, &QTabWidget::tabCloseRequested, this, &MainWindow::onTabCloseRequested);

    // 重新创建一个空白标签页
    newEmptyTab();
}

// 为标签页准备插件实例：路由结论来自 routeForFile()（内容优先 + 插件声明兜底），
// 实例一律由 createInstance() 独占创建（B2：不再回退到共享实例）。
bool MainWindow::resolvePlugin(const FILEINFO &info, const RouteDecision &route,
                               QString &sPluginPath,
                               PluginInterface* &pInterface, bool &bOwned)
{
    sPluginPath.clear();
    pInterface = nullptr;
    bOwned = false;

    sPluginPath = route.pluginPath;

    // 内容与后缀不一致时告诉用户"凭什么这么开"：自动判断必须可解释，
    // 否则用户会以为打开方式是随机的。
    if (!sPluginPath.isEmpty() && route.contentOverride)
    {
        const QString sExt = QFileInfo(info.sFilePath).suffix();
        statusBar()->showMessage(
            sExt.isEmpty()
                ? QStringLiteral("后缀无法判断，按文件内容识别为「%1」（%2），已用对应插件打开。")
                      .arg(route.formatName, route.basis)
                : QStringLiteral("按文件内容判断为「%1」（%2），与后缀不一致，已改用对应插件打开。")
                      .arg(route.formatName, route.basis),
            12000);
    }

    if (sPluginPath.isEmpty())
    {
        // 仍无法处理：告诉用户"这是什么 + 该怎么办"，而不是丢一个空白页。
        // route.detection 是路由时已经算好的识别结果，这里不必再读一遍文件头。
        const FormatResolver::Detection &det = route.detection;
        const QString sExt = QFileInfo(info.sFilePath).suffix();

        QMessageBox box(this);
        box.setWindowTitle(QStringLiteral("无法打开该文件"));
        box.setIcon(QMessageBox::Information);

        const QString sType = sExt.isEmpty() ? QStringLiteral("(无扩展名)")
                                             : QStringLiteral(".") + sExt;
        box.setText(QStringLiteral("「%1」的格式无法直接显示：%2")
                        .arg(info.sFileName, sType));

        QStringList infoLines;
        if (!det.displayName.isEmpty())
            infoLines << QStringLiteral("识别结果：%1").arg(det.displayName);
        if (!det.detail.isEmpty())
            infoLines << QStringLiteral("判断依据：%1").arg(det.detail);
        if (!det.advice.isEmpty())
            infoLines << QStringLiteral("\n建议：%1").arg(det.advice);
        if (!det.pluginExt.isEmpty())
            infoLines << QStringLiteral("\n识别为「%1」，但对应插件当前不可用（可能未部署或被禁用）。"
                                        "可在「插件设置」里检查。")
                             .arg(det.pluginExt);
        infoLines << QStringLiteral("\n本程序支持：docx/odt/rtf、xlsx/ods、pptx/odp、pdf、"
                                    "epub/fb2/mobi、文本/代码、图片、音视频。");
        box.setInformativeText(infoLines.join(QStringLiteral("\n")));

        QPushButton *bText   = box.addButton(QStringLiteral("以文本查看"), QMessageBox::AcceptRole);
        QPushButton *bHex    = box.addButton(QStringLiteral("以十六进制查看"), QMessageBox::AcceptRole);
        QPushButton *bList   = box.addButton(QStringLiteral("查看支持的格式…"), QMessageBox::ActionRole);
        QPushButton *bCancel = box.addButton(QStringLiteral("取消"), QMessageBox::RejectRole);
        // 默认按钮用「取消」：避免回车误触发"以文本查看"把二进制内容灌进文本视图
        box.setDefaultButton(bCancel);
        box.exec();

        QAbstractButton *clicked = box.clickedButton();
        Q_UNUSED(bCancel);   // 只是用于布局/默认按钮，点击判断不需要它
        if (clicked == bList)
        {
            showPluginSettings(1);
            return false;
        }
        if (clicked == bText)      sPluginPath = QStringLiteral("builtin://text");
        else if (clicked == bHex)  sPluginPath = QStringLiteral("builtin://hex");
        else return false;
    }

    if (sPluginPath.startsWith(QStringLiteral("builtin://")))
    {
        const BuiltinViewer::Mode mode = sPluginPath.endsWith(QStringLiteral("hex"))
                                       ? BuiltinViewer::HexMode : BuiltinViewer::TextMode;
        pInterface = new BuiltinViewer(mode);
        bOwned = true;
        return true;
    }

    PluginInterface *pTemplate = PluginManager::getInstance()->getInterface(sPluginPath);
    if (!pTemplate)
    {
        // 插件不可用：给出可读原因 + 部署自检结果，并支持「重试加载 / 以文本查看」。
        // 以前这里只有一行 qDebug，用户双击文件后没有任何反应（2026-11-17 修复）。
        qDebug() << "获取插件接口失败:" << sPluginPath;

        bool bUseBuiltinText = false;
        pTemplate = promptPluginUnavailable(sPluginPath, info.sFileName, &bUseBuiltinText);
        if (bUseBuiltinText)
        {
            // 退回到内置文本查看器（与「未找到插件」对话框里的兜底一致）
            pInterface = new BuiltinViewer(BuiltinViewer::TextMode);
            bOwned = true;
            return true;
        }
        if (pTemplate == nullptr)
            return false;
    }

    QString sCreateErr;
    pInterface = PluginManager::createInstanceFor(pTemplate, &sCreateErr);
    if (pInterface != nullptr)
    {
        bOwned = true;
        return true;
    }

    // B2：以前这里会「降级为共享实例」—— 同类型多标签页会共用同一内容区，
    // 打开 A 文件会把 B 标签页的内容顶掉，而且只留一行 qWarning。现在按打开失败处理，
    // 并给用户可读原因 + 文本查看兜底（与插件加载失败的处理保持一致）。
    qWarning() << "[Plugin] 无法创建插件实例:" << sPluginPath << sCreateErr;

    QMessageBox box(this);
    box.setWindowTitle(QStringLiteral("插件不可用"));
    box.setIcon(QMessageBox::Warning);
    box.setText(QStringLiteral("插件「%1」无法为「%2」创建查看实例。")
                    .arg(PluginDiagnostics::pluginDisplayName(sPluginPath),
                         info.sFileName.isEmpty() ? QStringLiteral("该文件") : info.sFileName));
    box.setInformativeText(sCreateErr + QStringLiteral(
        "\n\n每个插件都必须为每个标签页创建独立实例；退化成共享实例会让多个标签页互相覆盖内容，"
        "所以这里直接拒绝打开。"));
    QPushButton *bText = box.addButton(QStringLiteral("以文本查看"), QMessageBox::ActionRole);
    QPushButton *bCancel = box.addButton(QStringLiteral("取消"), QMessageBox::RejectRole);
    box.setDefaultButton(bText);
    box.exec();

    if (box.clickedButton() == bText)
    {
        pInterface = new BuiltinViewer(BuiltinViewer::TextMode);
        bOwned = true;
        return true;
    }
    Q_UNUSED(bCancel);
    return false;
}

// 插件不可用时的提示：显示 Qt 的原始错误 + 部署自检（缺哪些依赖、做什么用的），
// 并提供「重试加载 / 以文本查看 / 取消」三种处理。
// 返回：可用模板实例（重试成功后）；nullptr + *pbUseBuiltinText=true 表示改用文本查看；
//       nullptr + *pbUseBuiltinText=false 表示用户放弃。
PluginInterface *MainWindow::promptPluginUnavailable(const QString &sPluginPath,
                                                     const QString &sFileName,
                                                     bool *pbUseBuiltinText)
{
    if (pbUseBuiltinText) *pbUseBuiltinText = false;

    // C5：插件被用户禁用时，给出「去启用」的指引 —— 既不是加载失败，也不是"没插件"
    const QString sPluginId = PluginDiagnostics::pluginId(sPluginPath);
    if (!PluginManager::isPluginEnabled(sPluginId))
    {
        QMessageBox box(this);
        box.setWindowTitle(QStringLiteral("插件已禁用"));
        box.setIcon(QMessageBox::Information);
        box.setText(QStringLiteral("插件「%1」已被禁用，所以「%2」无法用默认方式打开。")
                        .arg(PluginDiagnostics::pluginDisplayName(sPluginPath),
                             sFileName.isEmpty() ? QStringLiteral("该文件") : sFileName));
        box.setInformativeText(QStringLiteral(
            "可以在「插件设置」里勾选启用并保存（会提示重新加载）。"));
        QPushButton *bText = box.addButton(QStringLiteral("以文本查看"), QMessageBox::ActionRole);
        QPushButton *bCancel = box.addButton(QStringLiteral("取消"), QMessageBox::RejectRole);
        box.setDefaultButton(bCancel);
        box.exec();
        if (box.clickedButton() == bText)
        {
            if (pbUseBuiltinText) *pbUseBuiltinText = true;
            m_setWarnedPlugins.insert(sPluginPath);
            m_setBuiltinTextPlugins.insert(sPluginPath);
            return nullptr;
        }
        m_setWarnedPlugins.insert(sPluginPath);
        return nullptr;
    }

    PluginManager *pMgr = PluginManager::getInstance();

    // 同一插件在本会话已经提示过、且用户当时选择放弃：不再反复弹窗打扰。
    // 但用户当时选择过“以文本查看”的，第二次仍应走文本兜底，而不是静默失败。
    if (m_setWarnedPlugins.contains(sPluginPath))
    {
        if (pbUseBuiltinText && m_setBuiltinTextPlugins.contains(sPluginPath))
            *pbUseBuiltinText = true;
        return nullptr;
    }

    for (;;)   // 用户点「重试加载」后仍失败时重新展示，直到成功或放弃
    {
        // 同时失败的其它插件（帮助判断是不是整批依赖都缺了，例如缺 Qt6Xml 会同时影响两个插件）
        QStringList sOtherFailed;
        for (const PluginManager::Failure &f : pMgr->failures())
            if (f.path != sPluginPath)
                sOtherFailed << PluginDiagnostics::pluginDisplayName(f.path);

        const QString sReport = PluginDiagnostics::buildReport(
                    sPluginPath, pMgr->failureFor(sPluginPath),
                    QCoreApplication::applicationDirPath(), sOtherFailed);

        QMessageBox box(this);
        box.setWindowTitle(QStringLiteral("插件不可用"));
        box.setIcon(QMessageBox::Warning);
        box.setText(QStringLiteral("无法加载插件「%1」，所以「%2」暂时打不开。")
                        .arg(PluginDiagnostics::pluginDisplayName(sPluginPath),
                             sFileName.isEmpty() ? QStringLiteral("该文件") : sFileName));
        box.setInformativeText(sReport);
        QPushButton *bRetry  = box.addButton(QStringLiteral("重试加载"), QMessageBox::AcceptRole);
        QPushButton *bText   = box.addButton(QStringLiteral("以文本查看"), QMessageBox::ActionRole);
        QPushButton *bCancel = box.addButton(QStringLiteral("取消"), QMessageBox::RejectRole);
        box.setDefaultButton(bRetry);
        box.exec();

        if (box.clickedButton() == bRetry)
        {
            pMgr->retryFailedPlugins();
            PluginInterface *pTemplate = pMgr->getInterface(sPluginPath);
            if (pTemplate != nullptr)
            {
                m_setWarnedPlugins.remove(sPluginPath);   // 恢复正常，允许以后再提示
                m_setBuiltinTextPlugins.remove(sPluginPath);
                return pTemplate;
            }
            continue;                                      // 还是不行：带着新错误再显示一次
        }
        if (box.clickedButton() == bText)
        {
            if (pbUseBuiltinText) *pbUseBuiltinText = true;
            m_setWarnedPlugins.insert(sPluginPath);
            m_setBuiltinTextPlugins.insert(sPluginPath);
            return nullptr;
        }
        Q_UNUSED(bCancel);
        m_setWarnedPlugins.insert(sPluginPath);
        return nullptr;
    }
}

// ================= 插件设置（状态 / 格式 / 启停 / 重载 合并入口） =================

// 「插件设置 → 支持的格式」页的正文：能打开什么、常见不支持格式该怎么办。
// 独立成函数：测试可以直接断言内容，不必弹模态框。
QString MainWindow::supportedFormatsText() const
{
    PluginManager *pMgr = PluginManager::getInstance();

    QStringList lines;
    lines << QStringLiteral("已安装插件及可打开的后缀：");
    for (const QString &sId : pMgr->knownPluginIds())
    {
        const QString sPath = pMgr->pluginPathForId(sId);
        if (sPath.isEmpty()) continue;
        QStringList exts = pMgr->extensionsOf(sPath);
        exts.sort();
        const bool bLoaded = pMgr->getInterface(sPath) != nullptr;
        QStringList dotted;
        for (const QString &e : exts) dotted << (QStringLiteral(".") + e);
        lines << QStringLiteral("  · %1%2：%3")
                     .arg(PluginDiagnostics::pluginDisplayName(sPath),
                          bLoaded ? QString() : QStringLiteral("（未加载：已禁用或依赖缺失）"),
                          dotted.isEmpty() ? QStringLiteral("（未声明后缀）")
                                           : dotted.join(QStringLiteral(" ")));
    }

    lines << QString();
    lines << QStringLiteral("打开方式由程序自动判断：优先按文件内容识别真实格式，"
                            "内容判断不出来时按插件声明的后缀；两者都不匹配才会提示。");
    lines << QStringLiteral("仍无法显示时，可以：");
    lines << QStringLiteral("  · 右键文件选择「以文本查看 / 以十六进制查看」（不会改动文件）");
    lines << QStringLiteral("  · 把它转换成受支持的格式（最省事）");

    lines << QString();
    lines << QStringLiteral("常见但本程序暂不支持的格式：");
    for (const auto &pair : FormatResolver::knownUnsupported())
        lines << QStringLiteral("  · %1：%2").arg(pair.first, pair.second);

    return lines.join(QLatin1Char('\n'));
}

// 「插件设置 → 状态与依赖」页的正文：已加载什么、谁没加载、缺哪些依赖、谁被禁用。
QString MainWindow::pluginStatusText() const
{
    PluginManager *pMgr = PluginManager::getInstance();
    const QString sAppDir = QCoreApplication::applicationDirPath();
    const QList<PluginManager::Failure> listFailures = pMgr->failures();
    const QList<PluginInterface *> listLoaded = pMgr->getAllInterfaces();

    QStringList lines;
    lines << QStringLiteral("程序目录：%1").arg(sAppDir);
    lines << QStringLiteral("运行环境：%1")
                 .arg(PluginDiagnostics::summaryLine(sAppDir, listFailures.size()));
    lines << QStringLiteral("已加载插件：%1 个").arg(listLoaded.size());

    // 加载失败的插件 + 原因
    if (!listFailures.isEmpty())
    {
        lines << QString();
        lines << QStringLiteral("加载失败的插件：");
        for (const PluginManager::Failure &f : listFailures)
        {
            QString sErr = f.error.trimmed();
            if (sErr.isEmpty()) sErr = QStringLiteral("（无错误信息）");
            lines << QStringLiteral("  · %1 —— %2")
                         .arg(PluginDiagnostics::pluginDisplayName(f.path), sErr);
        }
    }

    // 被禁用的插件（C5）：它们不加载，但要让用户知道"为什么这个格式打不开"
    const QStringList listDisabled = pMgr->disabledPluginIds();
    if (!listDisabled.isEmpty())
    {
        lines << QString();
        lines << QStringLiteral("已禁用（不会加载）：%1").arg(listDisabled.join(QStringLiteral("、")));
    }

    // 部署自检：缺哪些依赖、各自做什么用
    lines << QString();
    QString sCheckError;
    const QList<PluginDiagnostics::MissingFile> listMissing =
            PluginDiagnostics::missingFiles(sAppDir, &sCheckError);
    if (!sCheckError.isEmpty())
    {
        lines << QStringLiteral("部署自检：%1").arg(sCheckError);
    }
    else if (listMissing.isEmpty())
    {
        lines << QStringLiteral("部署自检：依赖齐全（清单：%1）")
                     .arg(PluginDiagnostics::manifestName());
    }
    else
    {
        lines << QStringLiteral("部署自检：缺少 %1 个依赖文件（清单：%2）")
                     .arg(listMissing.size()).arg(PluginDiagnostics::manifestName());
        for (const PluginDiagnostics::MissingFile &m : listMissing)
            lines << (m.hint.isEmpty()
                      ? QStringLiteral("  · %1").arg(m.path)
                      : QStringLiteral("  · %1 —— %2").arg(m.path, m.hint));
    }

    return lines.join(QLatin1Char('\n'));
}

void MainWindow::onActionPluginSettingsTriggered()
{
    showPluginSettings();
}

// 一个对话框装下原先四个入口：状态与依赖 / 支持的格式 / 启用禁用 + 重新加载。
// 以前「插件诊断 / 插件管理 / 支持的格式 / 重新加载插件」各占一个菜单项，内容互相
// 重叠；启停改完还得再点一次重载。现在保存后自动接上重载（重载本身会提示关标签页）。
void MainWindow::showPluginSettings(int nInitialTab)
{
    PluginManager *pMgr = PluginManager::getInstance();
    const QStringList listIds = pMgr->knownPluginIds();
    if (listIds.isEmpty())
    {
        QMessageBox::information(this, QStringLiteral("插件设置"),
                                 QStringLiteral("没有发现任何插件（plugins/ 目录为空或不可读）。"));
        return;
    }

    QDialog dlg(this);
    dlg.setWindowTitle(QStringLiteral("插件设置"));
    dlg.resize(720, 560);

    auto *pLayout = new QVBoxLayout(&dlg);
    auto *pTabs = new QTabWidget(&dlg);

    auto *pStatus = new QPlainTextEdit(pluginStatusText(), pTabs);
    pStatus->setReadOnly(true);
    pTabs->addTab(pStatus, QStringLiteral("状态与依赖"));

    auto *pFormats = new QPlainTextEdit(supportedFormatsText(), pTabs);
    pFormats->setReadOnly(true);
    pTabs->addTab(pFormats, QStringLiteral("支持的格式"));

    auto *pTogglePage = new QWidget(pTabs);
    auto *pToggleLayout = new QVBoxLayout(pTogglePage);
    auto *pTip = new QLabel(QStringLiteral(
        "勾选 = 启用。被禁用的插件不会加载，也不会接手它声明过的文件后缀。\n"
        "「保存」会应用勾选并重新加载（重新加载会关闭所有标签页）；"
        "只是新放了 DLL 的话，点「重新加载插件」即可。"), pTogglePage);
    pTip->setWordWrap(true);
    pToggleLayout->addWidget(pTip);

    auto *pList = new QListWidget(pTogglePage);
    for (const QString &sId : listIds)
    {
        auto *pItem = new QListWidgetItem(sId, pList);
        pItem->setFlags(pItem->flags() | Qt::ItemIsUserCheckable);
        pItem->setCheckState(PluginManager::isPluginEnabled(sId) ? Qt::Checked : Qt::Unchecked);
        if (!PluginManager::isPluginEnabled(sId))
            pItem->setToolTip(QStringLiteral("当前被禁用"));
    }
    pToggleLayout->addWidget(pList, 1);
    pTabs->addTab(pTogglePage, QStringLiteral("启用 / 禁用"));
    pTabs->setCurrentIndex(qBound(0, nInitialTab, pTabs->count() - 1));

    pLayout->addWidget(pTabs, 1);

    auto *pButtons = new QDialogButtonBox(&dlg);
    QPushButton *pReload = pButtons->addButton(QStringLiteral("重新加载插件…"),
                                               QDialogButtonBox::ActionRole);
    QPushButton *pSave   = pButtons->addButton(QStringLiteral("保存"),
                                               QDialogButtonBox::AcceptRole);
    QPushButton *pClose  = pButtons->addButton(QStringLiteral("关闭"),
                                               QDialogButtonBox::RejectRole);
    pLayout->addWidget(pButtons);
    connect(pSave, &QPushButton::clicked, &dlg, &QDialog::accept);
    connect(pClose, &QPushButton::clicked, &dlg, &QDialog::reject);
    // 自定义结果码 2：只重扫插件目录，不改启停
    connect(pReload, &QPushButton::clicked, &dlg, [&dlg]() { dlg.done(2); });

    const int nResult = dlg.exec();
    if (nResult == 2)
    {
        reloadAllPlugins();
        return;
    }
    if (nResult != QDialog::Accepted)
        return;

    bool bChanged = false;
    for (int i = 0; i < pList->count(); ++i)
    {
        QListWidgetItem *pItem = pList->item(i);
        const bool bWant = (pItem->checkState() == Qt::Checked);
        if (bWant != PluginManager::isPluginEnabled(pItem->text()))
        {
            PluginManager::setPluginEnabled(pItem->text(), bWant);
            bChanged = true;
        }
    }

    if (bChanged)
        reloadAllPlugins();   // 内部会先确认"关闭所有标签页"
    else
        statusBar()->showMessage(
            QStringLiteral("插件设置已保存（启停没有变化，无需重新加载）"), 5000);
}

void MainWindow::reloadAllPlugins()
{
    // 重新加载会卸载 DLL：必须先释放所有标签页里的插件实例，否则对象虚表悬空。
    // 只在确实有"打开过文件"的标签页时才需要确认（空标签页没有插件实例）。
    bool bAnyFileTab = false;
    for (const TabDoc &doc : m_vTabDocs)
        if (doc.hasFile) { bAnyFileTab = true; break; }

    if (bAnyFileTab)
    {
        if (QMessageBox::question(this, QStringLiteral("重新加载插件"),
                                  QStringLiteral("重新加载需要先关闭所有标签页（插件实例必须先释放）。\n是否继续？"),
                                  QMessageBox::Yes | QMessageBox::No,
                                  QMessageBox::No) != QMessageBox::Yes)
            return;
    }

    if (m_pCurrentInterface != nullptr)
    {
        ensureInstanceStopped(m_pCurrentInterface);
        m_pCurrentInterface = nullptr;
    }
    closeAllTabs();                        // 销毁标签页持有的插件实例
    pollStopUnsafeInstances();             // 后台线程可能已自行退出，先复查一次
    if (!m_vtDeferredInstances.isEmpty() || !m_setStopUnsafe.isEmpty())
    {
        // P0-3：仍有实例停播未确认 —— 卸载 DLL 会让后台线程执行已卸载代码
        QMessageBox::warning(this, QStringLiteral("重新加载插件"),
            QStringLiteral("有插件的后台播放未能确认停止，为避免访问已卸载的代码，\n"
                           "本次不重新加载插件。程序会每秒自动复查，也可稍后重试。"));
        return;
    }
    m_setWarnedPlugins.clear();
    m_setBuiltinTextPlugins.clear();
    m_setPluginWidgets.clear();

    PluginManager *pMgr = PluginManager::getInstance();
    pMgr->reloadPlugins();

    // 重新收集插件 widget 指针（清理标签页时据此避免误删）
    for (PluginInterface *pIface : pMgr->getAllInterfaces())
        m_setPluginWidgets.insert(pIface->getPluginWidget());

    QString sMsg = QStringLiteral("插件已重新加载：%1 个可用").arg(pMgr->getAllInterfaces().size());
    if (!pMgr->disabledPluginIds().isEmpty())
        sMsg += QStringLiteral("，%1 个被禁用").arg(pMgr->disabledPluginIds().size());
    const int nFailed = pMgr->failures().size();
    if (nFailed > 0)
        sMsg += QStringLiteral("，%1 个加载失败（见「插件设置」）").arg(nFailed);
    statusBar()->showMessage(sMsg, 15000);
}

// 新建标签页并打开选中的记录项文件（该页独占一个插件实例）
void MainWindow::openFileInNewTab(const FILEINFO &info)
{
    const RouteDecision route = routeForFile(info);
    QString sPluginPath;
    PluginInterface *pInterface = nullptr;
    bool bOwned = false;
    if (!resolvePlugin(info, route, sPluginPath, pInterface, bOwned))
        return;

    addFileTab(info, sPluginPath, pInterface, bOwned);
}

// 新建标签页并挂上已经创建好的实例：正常打开与「强制内置查看器」共用，
// 避免两处各写一遍 TabDoc 装配（字段漏一个就是标签页空白）。
void MainWindow::addFileTab(const FILEINFO &info, const QString &sPluginPath,
                            PluginInterface *pInterface, bool bOwned)
{
    TabDoc doc;
    doc.hasFile = true;
    doc.info = info;
    doc.pluginPath = sPluginPath;
    doc.interface = pInterface;
    doc.ownsInterface = bOwned;
    doc.needsRender = true;
    m_vTabDocs.append(doc);

    int newIdx = ui->tabWidget->addTab(new QWidget(), tabIconForFile(info.sFileName),
                                       info.sFileName);
    ui->tabWidget->setCurrentIndex(newIdx); // 触发 currentChanged -> activateTab
}

// 右键「以文本查看 / 以十六进制查看」：跳过自动路由，强制用内置查看器打开这一个文件。
// 这是原「插件映射」唯一值得保留的能力（看未识别的二进制、排查插件解析问题），
// 但只对当前文件生效、不写配置 —— 不会像映射那样永久改掉某个后缀的打开方式。
void MainWindow::openFileWithBuiltinViewer(const FILEINFO &info, bool bHex)
{
    auto *pViewer = new BuiltinViewer(bHex ? BuiltinViewer::HexMode : BuiltinViewer::TextMode);
    addFileTab(info,
               bHex ? QStringLiteral("builtin://hex") : QStringLiteral("builtin://text"),
               pViewer, true);
    statusBar()->showMessage(
        bHex ? QStringLiteral("已用内置十六进制查看器打开「%1」").arg(info.sFileName)
             : QStringLiteral("已用内置文本查看器打开「%1」").arg(info.sFileName), 5000);
}

// 在当前激活的标签页打开文件（无激活标签页时退化为新建）
void MainWindow::openFileInCurrentTab(const FILEINFO &info)
{
    int cur = ui->tabWidget->currentIndex();
    if (cur < 0 || cur >= m_vTabDocs.size())
    {
        openFileInNewTab(info);
        return;
    }

    TabDoc doc = m_vTabDocs.at(cur);   // 用副本，避免下方摘除/插入元素后引用失效

    // 当前标签页已打开别的文件时提示一下（双击默认替换当前页，新标签页需右键）
    if (doc.hasFile)
    {
        statusBar()->showMessage(
            tr("已在当前标签页替换为「%1」；如需另开，请右键选择「打开新标签页」")
                .arg(info.sFileName), 5000);
    }

    // 该页已持有同一插件的独占实例：直接复用，仅替换文件并重新渲染。
    // 路由结论先算一次（内容优先），避免"创建临时实例再删掉"的浪费
    //（媒体插件构造还会起解码线程，虽小但完全没必要）；同时也自动覆盖了
    //  ".ts" 这类后缀歧义：同一页可能从文本换成视频，结论不同就不会复用。
    const RouteDecision route = routeForFile(info);
    const QString &sQuick = route.pluginPath;
    if (doc.hasFile && doc.ownsInterface && doc.interface != nullptr
        && !sQuick.isEmpty() && !sQuick.startsWith(QStringLiteral("builtin://"))
        && doc.pluginPath == sQuick)
    {
        m_vTabDocs[cur].info = info;
        m_vTabDocs[cur].needsRender = true;
        ui->tabWidget->setTabText(cur, info.sFileName);
        ui->tabWidget->setTabIcon(cur, tabIconForFile(info.sFileName));
        activateTab(cur);
        return;
    }

    QString sPluginPath;
    PluginInterface *pInterface = nullptr;
    bool bOwned = false;
    if (!resolvePlugin(info, route, sPluginPath, pInterface, bOwned))
        return;

    // 该页已持有同一插件的独占实例：直接复用，仅替换文件并重新渲染
    if (doc.hasFile && doc.ownsInterface && bOwned
        && doc.interface != nullptr && doc.pluginPath == sPluginPath)
    {
        delete pInterface;             // 刚新建的实例不再需要
        m_vTabDocs[cur].info = info;
        m_vTabDocs[cur].needsRender = true;
        ui->tabWidget->setTabText(cur, info.sFileName);   // 标题跟随新文件
        ui->tabWidget->setTabIcon(cur, tabIconForFile(info.sFileName));
        activateTab(cur);
        return;
    }

    // 其它情况：该页需要换成新的插件实例（可能换了插件类型）。
    // P0-3：先确认旧实例的后台线程已退出。停播失败时**不** delete、不摘映射，
    // 实例转入延期清理（页面保持存活，随实例一起泄漏而不是悬空）。
    bool bOldSafeToDestroy = true;
    if (doc.interface != nullptr && doc.ownsInterface)
        bOldSafeToDestroy = ensureInstanceStopped(doc.interface);
    else if (doc.interface != nullptr && doc.interface == m_pCurrentInterface)
        stopCurrentPlaybackAndInvalidate();

    // 摘页并销毁旧的独占实例（连带其内部窗口）；共享实例与插件窗口不删
    QWidget *pOldPage = ui->tabWidget->widget(cur);
    m_vTabDocs.removeAt(cur);          // 先同步文档列表，使 removeTab 触发的 currentChanged 下标对齐
    ui->tabWidget->removeTab(cur);

    QWidget *pOldOwned = nullptr;
    if (doc.interface != nullptr && doc.ownsInterface)
    {
        pOldOwned = doc.interface->getPluginWidget();
        if (bOldSafeToDestroy)
            delete doc.interface;
        else
            m_vtDeferredInstances.append(doc.interface);
    }
    if (pOldPage != nullptr && pOldPage != pOldOwned && !m_setPluginWidgets.contains(pOldPage))
        delete pOldPage;

    // 复用原下标位置重建该页，并切回该页
    doc.hasFile = true;
    doc.info = info;
    doc.pluginPath = sPluginPath;
    doc.interface = pInterface;
    doc.ownsInterface = bOwned;
    doc.needsRender = true;
    m_vTabDocs.insert(cur, doc);

    ui->tabWidget->insertTab(cur, new QWidget(), tabIconForFile(info.sFileName), info.sFileName);
    ui->tabWidget->setCurrentIndex(cur); // 触发 currentChanged -> activateTab
    activateTab(cur);                    // 当前页未变化时 currentChanged 不触发，这里兜底显式激活
}

// P0-2：拖动标签后，标签顺序与 m_vTabDocs 必须保持一致。
// tabMoved 在移动完成后发出，被拖动的标签数据/窗口已经位于 to 位置；
// 按「该位置上的插件窗口」找到文档，再把它移动到同一位置。
void MainWindow::onTabBarTabMoved(int from, int to)
{
    if (from == to || from < 0 || to < 0)
        return;
    if (to >= m_vTabDocs.size() || from >= m_vTabDocs.size())
        return;

    // 被拖动的标签页（to 位置）对应的文档下标。空标签页没有插件窗口，
    // 直接使用 from（tabMoved 的语义就是同一元素从 from 移到 to）。
    int nDoc = -1;
    const QWidget *page = ui->tabWidget->widget(to);
    if (page != nullptr)
    {
        for (int i = 0; i < m_vTabDocs.size(); ++i)
        {
            if (m_vTabDocs.at(i).interface != nullptr
                && m_vTabDocs.at(i).interface->getPluginWidget() == page)
            {
                nDoc = i;
                break;
            }
        }
    }
    if (nDoc < 0)
        nDoc = from;
    if (nDoc == to)
        return;

    m_vTabDocs.move(nDoc, to);
}

// 标签下标 -> 文档下标（P0-2）。插件页用窗口指针定位，占位页回退到顺序下标。
int MainWindow::docIndexForTab(int tabIndex) const
{
    if (tabIndex < 0 || tabIndex >= ui->tabWidget->count())
        return -1;
    const QWidget *page = ui->tabWidget->widget(tabIndex);
    if (page != nullptr)
    {
        for (int i = 0; i < m_vTabDocs.size(); ++i)
        {
            if (m_vTabDocs.at(i).interface != nullptr
                && m_vTabDocs.at(i).interface->getPluginWidget() == page)
                return i;
        }
    }
    return (tabIndex < m_vTabDocs.size()) ? tabIndex : -1;
}

// P0-3：停止失败（返回 false）表示后台线程仍可能访问宿主的内存映射。
// 此时必须保留映射、插件实例和 loader，任何 unmap/delete/卸载都是 UAF。
bool MainWindow::ensureInstanceStopped(PluginInterface *iface)
{
    if (iface == nullptr)
        return true;

    // 已经确认停不下来：不再重复等待几十秒；实例保持延期清理状态。
    // 轮询器会继续用 isPlaybackStopped() 非阻塞地复查，后台线程退出后自动解除。
    if (m_setStopUnsafe.contains(iface))
    {
        if (m_stopUnsafePoll != nullptr && !m_stopUnsafePoll->isActive())
            m_stopUnsafePoll->start();
        return false;
    }

    if (m_pCurrentInterface == iface)
        m_pCurrentInterface = nullptr;

    if (!iface->stopPlaybackAndWait())
    {
        m_setStopUnsafe.insert(iface);
        if (m_stopUnsafePoll != nullptr && !m_stopUnsafePoll->isActive())
            m_stopUnsafePoll->start();
        qWarning() << "[Lifecycle] 插件停播未确认，保留映射与实例:"
                   << static_cast<const void *>(iface);
        return false;
    }

    releaseMappedInput(iface);
    return true;
}

bool MainWindow::ensureAllInstancesStopped()
{
    bool bAllSafe = true;
    QSet<PluginInterface *> seen;
    for (const TabDoc &doc : m_vTabDocs)
    {
        if (doc.interface == nullptr || seen.contains(doc.interface))
            continue;
        seen.insert(doc.interface);
        if (!ensureInstanceStopped(doc.interface))
            bAllSafe = false;
    }
    // 先前延期清理的实例也再试一次（成功就立即销毁）
    for (int i = m_vtDeferredInstances.size() - 1; i >= 0; --i)
    {
        PluginInterface *p = m_vtDeferredInstances.at(i);
        if (ensureInstanceStopped(p))
        {
            m_vtDeferredInstances.removeAt(i);
            delete p;
        }
        else
        {
            bAllSafe = false;
        }
    }
    return bAllSafe;
}

// 每秒调用一次（只在存在 unsafe 实例时运行）。只问插件“后台线程退出了吗”，
// 不调用 stopPlaybackAndWait()，因此不会把 GUI 再阻塞 30 秒。
void MainWindow::pollStopUnsafeInstances()
{
    if (m_setStopUnsafe.isEmpty())
    {
        if (m_stopUnsafePoll != nullptr)
            m_stopUnsafePoll->stop();
        return;
    }

    bool bRecovered = false;
    for (auto it = m_setStopUnsafe.begin(); it != m_setStopUnsafe.end(); )
    {
        PluginInterface *pIface = *it;
        if (pIface == nullptr)
        {
            it = m_setStopUnsafe.erase(it);
            continue;
        }
        // v7 契约：true = 此刻起不会再访问宿主映射/临时文件。
        if (pIface->isPlaybackStopped())
        {
            it = m_setStopUnsafe.erase(it);
            releaseMappedInput(pIface);   // 已不在 unsafe 集合，这里会真正 unmap
            bRecovered = true;
        }
        else
        {
            ++it;
        }
    }

    if (!bRecovered)
        return;

    // 延期实例里已经安全的，可以真正销毁（它们已从标签页摘除）。
    for (int i = m_vtDeferredInstances.size() - 1; i >= 0; --i)
    {
        PluginInterface *pIface = m_vtDeferredInstances.at(i);
        if (pIface == nullptr)
        {
            m_vtDeferredInstances.removeAt(i);
            continue;
        }
        if (m_setStopUnsafe.contains(pIface) || !pIface->isPlaybackStopped())
            continue;
        QWidget *pWidget = pIface->getPluginWidget();
        m_vtDeferredInstances.removeAt(i);
        if (pWidget != nullptr)
            m_setPluginWidgets.remove(pWidget);
        delete pIface;
    }

    if (m_setStopUnsafe.isEmpty() && m_vtDeferredInstances.isEmpty())
    {
        if (m_stopUnsafePoll != nullptr)
            m_stopUnsafePoll->stop();
        PluginManager::getInstance()->setUnloadEnabled(true);
        statusBar()->showMessage(
            tr("后台播放已停止，保存/重新加载插件功能已恢复"), 5000);
        qWarning() << "[Lifecycle] 停播超时实例已自行退出，已解除 unsafe 状态";
    }
}

// 销毁某个标签页：先把页面从 tabWidget 摘下，再释放该页独占的插件实例（连带销毁其内部窗口），
// 最后删除自建的占位页面。共享实例与插件（模板实例）窗口一律不删。
void MainWindow::destroyTabAt(int nTabIndex)
{
    if (nTabIndex < 0 || nTabIndex >= ui->tabWidget->count())
        return;
    const int index = docIndexForTab(nTabIndex);
    if (index < 0 || index >= m_vTabDocs.size())
        return;

    TabDoc doc = m_vTabDocs.at(index);
    QWidget *pPage = ui->tabWidget->widget(nTabIndex);

    // 先同步文档列表，再摘页：removeTab 可能触发 currentChanged -> activateTab，
    // 此时 m_vTabDocs 的下标必须已与 tabWidget 对齐。
    m_vTabDocs.removeAt(index);
    ui->tabWidget->removeTab(nTabIndex);   // removeTab 不删除页面 widget

    // 独占实例：delete 会连带 delete 其内部窗口（即该页面）
    QWidget *pOwnedWidget = nullptr;
    bool bSafeToDestroy = true;
    if (doc.interface != nullptr && doc.ownsInterface)
    {
        pOwnedWidget = doc.interface->getPluginWidget();
        // P0-3：先确认后台已退出。停止失败时**不**摘映射、不 delete、不卸载，
        // 把实例转入延期清理（页面 widget 由实例持有，保持存活）。
        bSafeToDestroy = ensureInstanceStopped(doc.interface);
        if (bSafeToDestroy)
        {
            delete doc.interface;
        }
        else
        {
            m_vtDeferredInstances.append(doc.interface);
            if (pPage != nullptr && pPage == pOwnedWidget)
                pPage = nullptr;   // 不删除实例拥有的页面
        }
    }

    // 其余需要删除的页面：自建占位页（不是插件窗口、也不是已随实例销毁的窗口）
    if (pPage != nullptr && pPage != pOwnedWidget && !m_setPluginWidgets.contains(pPage))
        delete pPage;
}

// 停止当前激活实例的后台播放；对带后台播放的插件，让其所在标签页在下次激活时重新渲染
void MainWindow::stopCurrentPlaybackAndInvalidate()
{
    PluginInterface *pOld = m_pCurrentInterface;
    m_pCurrentInterface = nullptr;
    if (pOld == nullptr)
        return;

    // P0-3：统一的停止入口。返回 false 时映射/实例都保持不动（已登记为延期清理），
    // 不再像旧代码那样各路径各判断一次。
    const bool bSafeToUnmap = ensureInstanceStopped(pOld);
    Q_UNUSED(bSafeToUnmap);

    if (!pOld->hasBackgroundPlayback())
        return;

    // 解码资源已被释放，切回该页需要重新渲染，否则只剩最后一帧/黑屏
    for (int i = 0; i < m_vTabDocs.size(); ++i)
    {
        if (m_vTabDocs[i].interface == pOld)
        {
            m_vTabDocs[i].needsRender = true;
            break;
        }
    }
}

// 大文件抽取到临时文件（流式，内存占用固定 1 MB）
bool MainWindow::extractToTempFile(const FILEINFO &info, QString &sOutPath, QString &sErr,
                                   bool *pbNoSpace)
{
    if (pbNoSpace) *pbNoSpace = false;
    // 缓存键：条目路径 + 长度 + 偏移（偏移变了说明归档内容区已重写）
    auto cacheKeyOf = [](const FILEINFO &i) {
        return QStringLiteral("%1|%2|%3")
            .arg(i.sFilePath).arg(i.nFileLen).arg(i.nContentPositon);
    };
    const QString key = cacheKeyOf(info);
    // 同一条目重复打开时复用已抽取的副本，避免为 10 GB 条目反复复制几十 GB
    auto cached = m_mapTempCache.constFind(key);
    if (cached != m_mapTempCache.constEnd()
        && QFileInfo::exists(cached.value())
        && QFileInfo(cached.value()).size() == info.nFileLen)
    {
        sOutPath = cached.value();
        // P1-8：命中后移到队尾，让淘汰顺序是真正的 LRU 而不是 FIFO
        m_listTempOrder.removeAll(sOutPath);
        m_listTempOrder.append(sOutPath);
        return true;
    }

    // B4：写临时副本前先看磁盘余量（预留 64 MiB 给其他临时文件与碎片）。
    // 空间不够就明确失败 —— 绝不退化成"整条读进内存"，几 GB 的条目会直接打爆进程。
    constexpr qint64 kSpaceMarginBytes = 64LL * 1024 * 1024;
    const qint64 nNeedBytes = info.nFileLen + kSpaceMarginBytes;
    qint64 nAvailableBytes = -1;
    if (!FileManager::hasFreeSpaceFor(QDir::tempPath(), nNeedBytes, &nAvailableBytes))
    {
        if (pbNoSpace) *pbNoSpace = true;
        sErr = QStringLiteral("临时目录所在磁盘空间不足：需要约 %1（含 64 MiB 预留），"
                              "当前可用 %2。\n临时目录：%3")
                   .arg(FwArc::formatSize(nNeedBytes),
                        FwArc::formatSize(qMax<qint64>(0, nAvailableBytes)),
                        QDir::tempPath());
        return false;
    }

    QDir dir(QDir::tempPath() + QStringLiteral("/FileWrapper"));
    if (!dir.exists() && !dir.mkpath(dir.absolutePath()))
    {
        sErr = QStringLiteral("无法创建临时目录: %1").arg(dir.absolutePath());
        return false;
    }
    // 上限淘汰：最多保留 8 个临时副本。P1-8：**活动标签页仍在引用的路径不得淘汰**
    // （接口契约是 path 在本次会话内一直有效；插件可能在后台线程继续读它）。
    static const int kMaxTempFiles = 8;
    QSet<QString> pinnedKeys;
    for (const TabDoc &doc : m_vTabDocs)
        if (doc.hasFile)
            pinnedKeys.insert(cacheKeyOf(doc.info));

    while (m_listTempOrder.size() >= kMaxTempFiles)
    {
        QString victim;
        for (const QString &cand : m_listTempOrder)
        {
            QString candKey;
            for (auto it = m_mapTempCache.constBegin(); it != m_mapTempCache.constEnd(); ++it)
            {
                if (it.value() == cand) { candKey = it.key(); break; }
            }
            if (!pinnedKeys.contains(candKey)) { victim = cand; break; }
        }
        if (victim.isEmpty())
            break;   // 全部被活动标签页引用：允许暂时超过上限，绝不删活动文件

        m_listTempOrder.removeAll(victim);
        if (QFile::remove(victim) || !QFileInfo::exists(victim))
        {
            m_setTempFiles.remove(victim);
            for (auto it = m_mapTempCache.begin(); it != m_mapTempCache.end(); )
            {
                if (it.value() == victim) it = m_mapTempCache.erase(it);
                else ++it;
            }
        }
        else
        {
            // 删除失败说明仍被插件占用：保留在 m_setTempFiles 里，等析构再清理
            qWarning() << "[TempCache] 临时文件仍被占用，保留到进程退出:" << victim;
        }
    }

    const QString name = QStringLiteral("fw_%1_%2_%3")
            .arg(QDateTime::currentMSecsSinceEpoch())
            .arg(++m_uTempSerial)
            .arg(info.sFileName);
    // 文件名里可能含路径分隔符（伪造的归档路径），只保留 basename
    const QString safe = QFileInfo(name).fileName();
    sOutPath = dir.absoluteFilePath(safe);

    if (!m_fileManager->extractEntryToTempFile(info.sFilePath, sOutPath, sErr))
        return false;
    m_setTempFiles.insert(sOutPath);
    m_mapTempCache.insert(key, sOutPath);
    m_listTempOrder.append(sOutPath);
    return true;
}

// 读取文件内容并送入插件渲染
void MainWindow::renderFile(const FILEINFO &info, PluginInterface *iface)
{
    if (iface == nullptr) return;
    iface->setSourceName(info.sFileName);

    // —— P1-2：大文件优先走「临时文件 + 路径」通道，插件可自行流式打开 ——
    if (info.nFileLen >= kLargeFileThreshold && iface->prefersFileByPath())
    {
        // v4：先尝试零拷贝内存映射（媒体插件支持）：直接把归档内容区映射为只读内存，
        // FFmpeg 按需读页，不必先复制出一整份临时文件（1.59GB 视频不再"准备"1.59GB）。
        // 注意：新增未保存的文件没有归档内偏移，不能映射。
        if (!m_fileManager->archivePath().isEmpty()
            && !m_fileManager->isPendingEntry(info.sFilePath))
        {
            auto *pArc = new QFile(m_fileManager->archivePath());
            if (pArc->open(QIODevice::ReadOnly))
            {
                uchar *pMapped = pArc->map(info.nContentPositon, info.nFileLen);
                bool bMappedAccepted = false;
                QString sMappedWhy;
                if (pMapped != nullptr)
                {
                    bMappedAccepted = iface->sendFileMapped(
                        reinterpret_cast<char *>(pMapped), info.nFileLen, info.sFileName);
                    // v6：插件可能已经**认出**文件但播不了（没有音频输出设备、解码器
                    // 不支持该编码…）。这种失败再抽一遍临时文件重试毫无意义，还可能白写
                    // 几 GB；必须把插件给的真实原因直接说出去。
                    if (!bMappedAccepted)
                        sMappedWhy = iface->lastError().trimmed();
                }
                // pMapped == nullptr 时插件根本没被调用：不能去读 lastError()，
                // 那可能是上一个文件留下的原因，会误报并跳过本可成功的临时文件回退。
                if (bMappedAccepted)
                {
                    // 先交给插件开始播放；CRC 改为后台校验，不阻塞首帧。
                    auto sp = QSharedPointer<MappedInput>::create();
                    sp->pFile = pArc;
                    sp->pData = pMapped;
                    sp->nLen = info.nFileLen;
                    m_mapMappedInputs.insert(iface, sp);

                    quint32 nExpected = 0;
                    // 缓存键带上条目偏移/长度：删除后重新添加"同名但不同内容"的条目时，
                    // 不能复用之前那条"已校验"的结论（只清归档切换还不够）。
                    const QString sCrcKey = QStringLiteral("%1|%2|%3")
                            .arg(info.sFilePath).arg(info.nContentPositon).arg(info.nFileLen);
                    if (m_fileManager->contentCrcFor(info.sFilePath, &nExpected)
                        && !m_setMappedCrcVerified.contains(sCrcKey)
                        && !m_setMappedCrcPending.contains(sCrcKey))
                    {
                        m_setMappedCrcPending.insert(sCrcKey);
                        checkMappedCrcAsync(sCrcKey, info.sFilePath, info.nContentPositon,
                                            info.nFileLen, nExpected);
                    }
                    return;
                }
                if (pMapped != nullptr)
                    pArc->unmap(pMapped);
                pArc->close();
                delete pArc;
                pArc = nullptr;
                if (!sMappedWhy.isEmpty())
                {
                    releaseMappedInput(iface);
                    syncActionStates();
                    onFileManagerWarning(MainWindow::pluginFailureText(info.sFileName, sMappedWhy));
                    return;
                }
            }
            if (pArc != nullptr)
            {
                delete pArc;
                pArc = nullptr;
            }
        }

        QString sTemp, sErr;
        bool bNoSpace = false;
        // 大文件抽取可能持续数秒：显示可取消的进度（复用 FileManager 的 progressChanged）
        QProgressDialog dlgProgress(tr("正在准备大文件..."), tr("取消"), 0, 100, this);
        dlgProgress.setWindowTitle(tr("准备文件"));
        dlgProgress.setWindowModality(Qt::WindowModal);
        dlgProgress.setMinimumDuration(0);
        dlgProgress.setAutoClose(false);
        dlgProgress.setAutoReset(false);
        dlgProgress.setValue(0);
        bool bDummy = false;
        QString sDummy;
        beginProgressOperation(&dlgProgress, &bDummy, &sDummy);
        dlgProgress.show();
        QApplication::processEvents();
        const bool bExtracted = extractToTempFile(info, sTemp, sErr, &bNoSpace);
        dlgProgress.close();
        endProgressOperation();
        if (bExtracted)
        {
            if (iface->sendFile(PluginFile::fromPath(sTemp, info.sFileName)))
            {
                // 新的播放已建立（旧后台播放已被插件停止）：释放旧映射
                releaseMappedInput(iface);
                return;
            }
            // v6：插件给了真实原因说明它**读到了**数据但显示/播放不了
            // （文件损坏、没有音频输出设备、编码不支持…）。这不是"路径通道打不开"，
            // 不能再拿「文件过大…查看器无法通过文件路径读取它」敷衍用户。
            const QString sPathWhy = iface->lastError().trimmed();
            if (!sPathWhy.isEmpty())
            {
                releaseMappedInput(iface);
                syncActionStates();
                onFileManagerWarning(MainWindow::pluginFailureText(info.sFileName, sPathWhy));
                return;
            }
            // P1-7：插件明确拒绝了路径载体。大文件不得再回退到「整块读入内存」——
            // 偏好路径的插件反而会因此绕过大小闸门，损坏/加密文件会把进程打爆。
            iface->sendFileData(nullptr, 0);
            releaseMappedInput(iface);
            syncActionStates();
            QMessageBox::warning(this, tr("打不开文件"),
                tr("文件过大（%1），查看器无法通过文件路径读取它。\n"
                   "为避免把整个文件读入内存，已停止打开。\n\n"
                   "可以先用「导出文件」或「分割文件」处理该文件。")
                    .arg(FwArc::formatSize(info.nFileLen)));
            return;
        }
        else if (bNoSpace)
        {
            // B4：磁盘放不下时**不能**退回内存通道（几 GB 的条目会直接把进程打爆），
            // 直接给用户一个能照做的提示，并清掉内容区。
            QMessageBox::warning(this, QStringLiteral("打不开文件"), sErr);
            iface->sendFileData(nullptr, 0);
            releaseMappedInput(iface);
            return;
        }
        else if (sErr.contains(tr("取消")))
        {
            // 用户取消：清空内容区并退出，不要退回内存通道把大文件读进内存
            iface->sendFileData(nullptr, 0);
            releaseMappedInput(iface);
            syncActionStates();
            return;
        }
        else
        {
            // P1-7：路径准备失败的其它原因（临时目录不可写、映射失败、内容无效…）
            // 同样不回退内存，直接把原因呈现给用户。
            iface->sendFileData(nullptr, 0);
            releaseMappedInput(iface);
            syncActionStates();
            QMessageBox::warning(this, tr("打不开文件"),
                tr("文件较大（%1），未能准备流式读取通道：\n%2\n\n"
                   "为避免一次性读入内存，已停止打开。")
                    .arg(FwArc::formatSize(info.nFileLen),
                         sErr.isEmpty() ? tr("未知原因") : sErr));
            return;
        }
    }

    // 非路径型查看器没有流式通道：>=128MB 时不要先整块读入再让插件拒绝。
    if (info.nFileLen >= kLargeFileThreshold && !iface->prefersFileByPath())
    {
        QMessageBox::warning(this, tr("打不开文件"),
            tr("文件过大（%1），当前查看器不支持流式读取。\n"
               "请先使用「导出文件」或「分割文件」处理该文件。")
                .arg(FwArc::formatSize(info.nFileLen)));
        iface->sendFileData(nullptr, 0);
        return;
    }

    // C2：内存通道直接读进 QByteArray（不再经过 new char[] + delete[] 的中转），
    // 通过 PluginFile 交给插件；迁移到 sendFile() 的插件可以零拷贝取用。
    QString sReadErr;
    QByteArray data = m_fileManager->entryContent(info.sFilePath, &sReadErr);
    if (data.isEmpty())
    {
        // 区分「0 字节空文件」与「读取失败」，并给出可读提示（P3-2 / P1-2）
        // 区分「0 字节空文件」与「读取失败」：entryContent 失败时会给 sReadErr，
        // 为空就说明确实是 0 字节（不要再去看 lastError()，那是别的操作留下的陈旧信息）。
        if (sReadErr.isEmpty())
        {
            // 真正的 0 字节文件不是“读取失败”：传一个非空指针的 0 长度缓冲，
            // 内置查看器显示空内容，状态栏明确提示“文件为空”。
            char cEmpty = 0;
            iface->sendFileData(&cEmpty, 0);
            statusBar()->showMessage(tr("文件为空（0 字节）：%1").arg(info.sFileName), 5000);
        }
        else
        {
            onFileManagerWarning(QStringLiteral("打开 %1 失败：%2").arg(info.sFileName, sReadErr));
            iface->sendFileData(nullptr, 0);
        }
        releaseMappedInput(iface);
        return;
    }
    // 插件在 sendFileData 返回前会同步复制/使用数据，返回后可安全释放。
    // sendFile() 返回 false 表示插件明确放弃（格式不支持/解码失败/环境不满足），
    // 宿主必须给出可见提示，不能只留空白画面。
    if (!iface->sendFile(PluginFile::fromMemory(std::move(data), info.sFileName)))
    {
        // v6：插件回报真实原因时按原因说。以前不管什么原因，界面上一律是
        // 「插件无法显示该文件」—— 用户以为是文件坏了，实际可能只是没插耳机。
        const QString sWhy = iface->lastError().trimmed();
        // 拿到原因说明插件已经把说明写在自己的界面上了，这时**不能**再清屏，
        // 否则那句说明会连同画面一起被抹掉（用户只看得到一块黑屏）。
        // 老插件不报原因，仍按原逻辑复位内容区。
        if (sWhy.isEmpty())
            iface->sendFileData(nullptr, 0);
        syncActionStates();
        onFileManagerWarning(MainWindow::pluginFailureText(info.sFileName, sWhy));
    }
    // 新内容已生效：旧的内存映射不再被插件引用，可以释放（Windows 下不释放会阻止归档重写）
    releaseMappedInput(iface);
}

void MainWindow::checkMappedCrcAsync(const QString &sCrcKey, const QString &sPath,
                                     qint64 nPosition, qint64 nLen, quint32 nExpected)
{
    // 后台线程自己打开归档读取内容区：QFile 在工作线程创建并销毁，不与主线程
    // 共享 QObject。不捕获映射对象，避免映射在工作线程析构（跨线程 unmap + delete）。
    // 代价是保存前必须等它读完：Windows 下仍打开的归档句柄会让 rename 失败。
    const QString sArchive = m_fileManager->archivePath();
    const quint64 nEpoch = m_nArchiveEpoch;
    const QSharedPointer<MappedCrcGate> gate = m_crcGate;
    gate->cancelRequested.store(false, std::memory_order_relaxed);
    {
        QMutexLocker locker(&gate->mutex);
        ++gate->inFlight;
    }
    QPointer<MainWindow> self(this);
    try
    {
        std::thread([self, gate, sArchive, sCrcKey, sPath, nPosition, nLen, nExpected, nEpoch]() {
            bool bOk = false;
            bool bReadOk = false;
            bool bCancelled = false;
            try
            {
                QFile f(sArchive);
                if (f.open(QIODevice::ReadOnly) && f.seek(nPosition))
                {
                    FwArc::Crc32 crc;
                    QByteArray buf(1 << 20, '\0');
                    qint64 nLeft = nLen;
                    bReadOk = true;
                    while (nLeft > 0)
                    {
                        // 保存需要"归档句柄已关闭"，不需要校验结论：主线程等待时置位，
                        // 这里最多再读一个块就退出，避免 GUI 被整段读取卡住。
                        if (gate->cancelRequested.load(std::memory_order_relaxed))
                        {
                            bCancelled = true;
                            bReadOk = false;
                            break;
                        }
                        const qint64 n = f.read(buf.data(), qMin<qint64>(qint64(buf.size()), nLeft));
                        if (n <= 0) { bReadOk = false; break; }
                        crc.update(buf.constData(), n);
                        nLeft -= n;
                    }
                    bOk = !bCancelled && bReadOk && nLeft == 0 && crc.value() == nExpected;
                }
            }
            catch (...)
            {
                // 线程内的分配失败等异常不能逃出 detached 线程（会 terminate）
                bReadOk = false;
            }
            // 句柄已随局部 QFile 关闭：计数器归零后主线程保存/改名才安全。
            {
                QMutexLocker locker(&gate->mutex);
                if (--gate->inFlight == 0)
                    gate->idle.wakeAll();
            }
            if (self.isNull()) return;
            // 投递到 qApp 而不是 MainWindow：窗口析构与投递之间没有原子性，
            // 用 qApp 作 context 可避免向已释放的窗口投递事件。
            // 应用对象已经销毁（极端退出顺序）时直接放弃结果。
            QCoreApplication *pApp = QCoreApplication::instance();
            if (pApp == nullptr) return;
            QMetaObject::invokeMethod(pApp, [self, sCrcKey, sPath, bOk, bReadOk, bCancelled, nEpoch]() {
                if (self.isNull()) return;
                if (nEpoch != self->m_nArchiveEpoch) return;   // 归档已切换：丢弃过期结果
                self->m_setMappedCrcPending.remove(sCrcKey);
                if (bCancelled)
                    return;   // 保存触发的主动取消：不下结论、不告警，下次打开会重新校验
                if (bOk)
                    self->m_setMappedCrcVerified.insert(sCrcKey);
                else if (!bReadOk)
                    self->onFileManagerWarning(tr("读取归档内容失败，未能完成校验：%1")
                                                   .arg(QFileInfo(sPath).fileName()));
                else
                    self->onFileManagerWarning(tr("内容校验失败（CRC32 不匹配），文件可能已损坏：%1")
                                                   .arg(QFileInfo(sPath).fileName()));
            }, Qt::QueuedConnection);
        }).detach();
    }
    catch (const std::exception &)
    {
        // 线程创建失败：回滚计数与 pending，否则保存入口会永久等待。
        {
            QMutexLocker locker(&gate->mutex);
            if (--gate->inFlight == 0)
                gate->idle.wakeAll();
        }
        m_setMappedCrcPending.remove(sCrcKey);
        onFileManagerWarning(tr("无法启动后台校验线程，本次跳过内容校验：%1")
                                 .arg(QFileInfo(sPath).fileName()));
    }
}

bool MainWindow::waitForMappedCrc(int nTimeoutMs)
{
    const QSharedPointer<MappedCrcGate> gate = m_crcGate;
    // 先请求取消：保存只需要归档句柄关闭，等整段 CRC 读完只会让 GUI 无谓冻结。
    gate->cancelRequested.store(true, std::memory_order_relaxed);
    QElapsedTimer timer;
    timer.start();
    QMutexLocker locker(&gate->mutex);
    while (gate->inFlight > 0)
    {
        const qint64 nLeft = qint64(nTimeoutMs) - timer.elapsed();
        if (nLeft <= 0)
            return false;
        if (!gate->idle.wait(&gate->mutex, QDeadlineTimer(nLeft)))
            return false;
    }
    return true;
}

void MainWindow::releaseMappedInput(PluginInterface *iface)
{
    if (iface == nullptr)
        return;
    // P0-3 的不变量：停播未确认的实例，其映射可能仍被后台解码线程读取。
    // releaseAllMappedInputs() 已经这样保护；releaseMappedInput() 的调用点
    // （打开失败/换文件/关闭标签）也必须一致，否则一次 stopPlaybackAndWait
    // 超时后 unmap 会让卡住的 worker 读到悬空内存。
    if (m_setStopUnsafe.contains(iface))
    {
        qWarning() << "[Lifecycle] 停播未确认，保留内存映射不释放";
        return;
    }
    m_mapMappedInputs.remove(iface);
}

void MainWindow::releaseAllMappedInputs()
{
    // 只在关闭/析构路径调用：此时标签页实例已或即将销毁，不能再解引用
    // 插件指针；正常切页的安全释放由 stopCurrentPlaybackAndInvalidate 负责。
    // P0-3：停播未确认的实例仍可能访问映射，必须保留。
    for (auto it = m_mapMappedInputs.begin(); it != m_mapMappedInputs.end(); )
    {
        if (m_setStopUnsafe.contains(it.key()))
            ++it;
        else
            it = m_mapMappedInputs.erase(it);
    }
}

// 激活某个标签页：挂载该页独占的插件窗口，并按需渲染其文件
void MainWindow::activateTab(int index)
{
    if (index < 0 || index >= ui->tabWidget->count())
    {
        // 索引无效（如最后一个标签页被移除时的 currentChanged(-1)）：
        // 仍要停止上一个实例的后台播放，避免无人接管的解码线程继续运行
        stopCurrentPlaybackAndInvalidate();
        return;
    }

    // P0-2：按标签页上的实际窗口找文档，而不是盲信下标。
    const int nDocIndex = docIndexForTab(index);
    if (nDocIndex < 0 || nDocIndex >= m_vTabDocs.size())
    {
        stopCurrentPlaybackAndInvalidate();
        return;
    }
    TabDoc &doc = m_vTabDocs[nDocIndex];

    // 空白标签页：停止上一个实例的后台播放，页面保持空白
    if (!doc.hasFile || doc.interface == nullptr)
    {
        stopCurrentPlaybackAndInvalidate();
        return;
    }

    QWidget *w = doc.interface->getPluginWidget();

    // 挂载：保证本页展示的正是本标签页独占的窗口。
    // 摘页/插页会改变 currentIndex 并可能递归触发 currentChanged，
    // 这里先屏蔽信号完成挂载并锁定当前页，避免重入。
    if (ui->tabWidget->widget(index) != w)
    {
        QWidget *pOldPage = ui->tabWidget->widget(index);
        {
            QSignalBlocker blocker(ui->tabWidget);
            ui->tabWidget->removeTab(index);
            ui->tabWidget->insertTab(index, w, tabIconForFile(doc.info.sFileName), doc.info.sFileName);
            ui->tabWidget->setCurrentIndex(index);
        }
        // 仅删除自建占位页；插件窗口（本页独占或插件共享）不删
        if (pOldPage != nullptr && pOldPage != w && !m_setPluginWidgets.contains(pOldPage))
            delete pOldPage;
    }

    // 切换实例时停止上一个实例的后台播放（同类型的两个标签页也是各自独立的实例）
    if (m_pCurrentInterface && m_pCurrentInterface != doc.interface)
        stopCurrentPlaybackAndInvalidate();
    m_pCurrentInterface = doc.interface;

    // 渲染：首次打开、文件被替换，或带后台播放的实例被停播后（缓存已失效）时，
    // 重新读取文件内容并送入插件。
    // 放到下一轮事件循环执行：先让标签页/标题立即切过去，避免双击后整个窗口
    // 在渲染期间（大图/大日志/首次打开媒体可达数百毫秒）毫无响应。
    if (doc.needsRender)
    {
        const quint64 nGen = ++m_nRenderGen;
        const int nTabIndex = index;
        const QString sExpectPath = doc.info.sFilePath;
        QTimer::singleShot(0, this, [this, nTabIndex, nGen, sExpectPath]() {
            if (nGen != m_nRenderGen)
                return;                                  // 已被更新的切换取消
            if (ui->tabWidget->currentIndex() != nTabIndex)
                return;                                  // 用户已切走，保留 needsRender
            // P0-2：延迟回调里窗口可能已被拖动/关闭，重新按实际窗口定位文档。
            const int nDoc = docIndexForTab(nTabIndex);
            if (nDoc < 0 || nDoc >= m_vTabDocs.size())
                return;
            TabDoc &d = m_vTabDocs[nDoc];
            if (!d.needsRender || d.interface == nullptr
                || d.info.sFilePath != sExpectPath)
                return;
            d.needsRender = false;
            QApplication::setOverrideCursor(Qt::WaitCursor);
            statusBar()->showMessage(tr("正在打开 %1…").arg(d.info.sFileName));
            renderFile(d.info, d.interface);
            QApplication::restoreOverrideCursor();
            // 只清理“正在打开…”；renderFile 失败时写的错误消息要保留。
            if (statusBar()->currentMessage().startsWith(tr("正在打开 ")))
                statusBar()->clearMessage();
        });
    }
}

// 用户点击标签页关闭按钮
void MainWindow::onTabCloseRequested(int index)
{
    if (index < 0 || index >= ui->tabWidget->count())
        return;
    const int nDoc = docIndexForTab(index);
    if (nDoc < 0 || nDoc >= m_vTabDocs.size())
        return;

    // 关闭前先停止该页实例（P0-3：停不下来时 destroyTabAt 会转入延期清理）
    PluginInterface *pIface = m_vTabDocs.at(nDoc).interface;
    if (pIface != nullptr)
        ensureInstanceStopped(pIface);

    destroyTabAt(index);   // 摘页 + 释放该页独占的插件实例

    if (ui->tabWidget->count() == 0)
        newEmptyTab();     // 始终保持至少一个标签页
}

// 切换标签页
void MainWindow::onTabCurrentChanged(int index)
{
    activateTab(index);
}

// 目录节点的完整路径：把从根到该节点的文本逐段拼起来。
// 单链压缩后的节点文本本身可能含 '/'（如 "sub/deep"），直接按段拼接仍然正确。
static QString folderPathOfItem(const QStandardItem *pItem)
{
    QStringList segs;
    for (const QStandardItem *p = pItem; p != nullptr; p = p->parent())
        segs.prepend(p->text());
    return normalizePath(segs.join('/'));
}

// 目录子树字节合计：只在模型里没有缓存大小（BytesRole）时兜底使用。
// 始终按叶子的 FILEINFO 现算，避免读到一个过期/缺失的列值。
// 用显式栈而不是递归：归档路径允许很深（MAX_PATH_LEN/每段 255），递归遍历
// 极端输入会把 native 栈耗尽（与 FileListModel::sort 的 kMaxDepth 同一个理由）。
static qint64 folderBytesOfItem(const QStandardItem *pDir)
{
    qint64 nTotal = 0;
    QVector<const QStandardItem *> stack;
    if (pDir != nullptr)
        stack.append(pDir);
    while (!stack.isEmpty())
    {
        const QStandardItem *pCurrent = stack.takeLast();
        for (int r = 0; r < pCurrent->rowCount(); ++r)
        {
            const QStandardItem *pChild = pCurrent->child(r, 0);
            if (pChild == nullptr)
                continue;
            if (isFileLeafItem(pChild))
                nTotal += pChild->data(FileListModel::InfoRole).value<FILEINFO>().nFileLen;
            else
                stack.append(pChild);
        }
    }
    return nTotal;
}

// 文件列表右键菜单（树形模式）
void MainWindow::onTreeViewCustomContextMenu(const QPoint &pos)
{
    showFileListContextMenu(m_tvFilePath, pos);
}

// 文件列表右键菜单（缩略图模式）
void MainWindow::onThumbnailViewCustomContextMenu(const QPoint &pos)
{
    showFileListContextMenu(m_lvFilePath, pos);
}

// 右键菜单的共用实现：两种视图只是 indexAt()/viewport 不同，菜单内容与副作用一致。
void MainWindow::showFileListContextMenu(QAbstractItemView *pView, const QPoint &pos)
{
    if (pView == nullptr || !m_fileManager->isArchiveLoaded())
        return;

    const QModelIndex index = pView->indexAt(pos);
    if (!index.isValid())
        return;

    // 右键同时把该行设为当前行：选中态与"属性/打开"作用的对象一致，也方便随后的删除/导出
    const QModelIndex itemIndex = index.siblingAtColumn(0);
    pView->setCurrentIndex(itemIndex);

    QMenu *pMenu = buildFileListContextMenu(itemIndex);
    if (pMenu == nullptr)
        return;
    pMenu->exec(pView->viewport()->mapToGlobal(pos));
    delete pMenu;
}

// 构造文件列表右键菜单（不 exec，便于单元测试检查菜单项）：
//   文件：打开 / 打开新标签页 / ─── / 标记… / 以文本查看 / 以十六进制查看 / ─── / 属性
//   目录：属性（目录不能"打开"，双击只做展开/收起）
QMenu *MainWindow::buildFileListContextMenu(const QModelIndex &index)
{
    if (!index.isValid() || m_hModelFilePath == nullptr)
        return nullptr;

    // 右键可能落在"大小/类型"列：统一取同一行的第 0 列业务项
    const QModelIndex itemIndex = index.siblingAtColumn(0);
    QStandardItem *pItem = m_hModelFilePath->itemFromIndex(itemIndex);
    if (pItem == nullptr)
        return nullptr;

    const bool bFile = isFileLeafItem(pItem);
    const bool bDir = pItem->data(FileListModel::IsDirRole).toBool();
    if (!bFile && !bDir)
        return nullptr;

    const FILEINFO info = bFile ? pItem->data(FileListModel::InfoRole).value<FILEINFO>()
                                : FILEINFO();

    auto *pMenu = new QMenu(this);
    if (bFile)
    {
        QAction *pOpen = pMenu->addAction(tr("打开"));
        connect(pOpen, &QAction::triggered, this, [this, info]() {
            openFileInCurrentTab(info);
        });

        QAction *pOpenNew = pMenu->addAction(tr("打开新标签页"));
        connect(pOpenNew, &QAction::triggered, this, [this, info]() {
            openFileInNewTab(info);
        });
        pMenu->addSeparator();

        // v4 标记：改的是归档条目本身，保存后随 .fwda 一起分发
        QAction *pTags = pMenu->addAction(tr("标记…"));
        connect(pTags, &QAction::triggered, this, [this, info]() {
            editEntryTags(info);
        });
        pMenu->addSeparator();

        // 不合格式也想要"看一眼内容"时的兜底：只作用于这一个文件，
        // 不改变后缀路由（原「插件映射」的手工兜底由它取代）。
        QAction *pText = pMenu->addAction(tr("以文本查看"));
        connect(pText, &QAction::triggered, this, [this, info]() {
            openFileWithBuiltinViewer(info, false);
        });

        QAction *pHex = pMenu->addAction(tr("以十六进制查看"));
        connect(pHex, &QAction::triggered, this, [this, info]() {
            openFileWithBuiltinViewer(info, true);
        });
        pMenu->addSeparator();
    }

    QAction *pProp = pMenu->addAction(tr("属性"));
    if (bFile)
    {
        connect(pProp, &QAction::triggered, this, [this, info]() { showFileProperty(info); });
    }
    else
    {
        // 菜单可能停留一段时间：用持久索引，期间列表被重建也不会留下悬空指针
        const QPersistentModelIndex persist(itemIndex);
        connect(pProp, &QAction::triggered, this, [this, persist]() {
            if (persist.isValid())
                showFolderProperty(persist);
        });
    }
    return pMenu;
}

// 「标记…」：编辑一个归档条目的 v4 标记。
// 候选列表来自整个归档（复用已有分类），确认后由 FileManager 归一化并置脏；
// 视图必须重建：当前筛选词可能正好是某个标记，改完标记后该行可能应该出现/消失。
void MainWindow::editEntryTags(const FILEINFO &info)
{
    if (!m_fileManager->isArchiveLoaded())
        return;

    QStringList tags = info.listTags;
    if (!DlgEntryTags::edit(this, info.sFileName, m_fileManager->allTags(),
                            info.listTags, &tags))
    {
        return;   // 用户取消：不动归档
    }

    QString sErr;
    if (!m_fileManager->setEntryTags(info.sFilePath, tags, &sErr))
    {
        QMessageBox::warning(this, tr("设置标记失败"),
                             tr("「%1」的标记未保存：\n%2").arg(info.sFileName, sErr));
        return;
    }

    refreshFilePathView();
    syncActionStates();   // 保存按钮与标题的 "*" 跟随脏标记
    statusBar()->showMessage(
        tags.isEmpty() ? tr("已清除「%1」的标记").arg(info.sFileName)
                       : tr("已设置「%1」的标记：%2")
                             .arg(info.sFileName, tags.join(QStringLiteral("、"))),
        5000);
}

// 统计目录子树里的文件数与子文件夹数（目录节点本身不计入）。
// 与 folderBytesOfItem 一样用显式栈，避免极深目录把栈耗尽。
void MainWindow::countFolderItems(const QStandardItem *pDir, int &nFiles, int &nDirs)
{
    QVector<const QStandardItem *> stack;
    if (pDir != nullptr)
        stack.append(pDir);
    while (!stack.isEmpty())
    {
        const QStandardItem *pCurrent = stack.takeLast();
        for (int r = 0; r < pCurrent->rowCount(); ++r)
        {
            const QStandardItem *pChild = pCurrent->child(r, 0);
            if (pChild == nullptr)
                continue;
            if (isFileLeafItem(pChild))
            {
                ++nFiles;
            }
            else if (pChild->data(FileListModel::IsDirRole).toBool())
            {
                ++nDirs;
                stack.append(pChild);
            }
        }
    }
}

// 「属性」：归档内的文件条目（先收集字段，再交给对话框展示）
DlgFileProperty::Data MainWindow::filePropertyData(const FILEINFO &info) const
{
    DlgFileProperty::Data data;
    data.sName = info.sFileName;
    data.sType = DlgFileProperty::typeTextForFile(info.sFileName);
    data.nSize = info.nFileLen;

    const QString sPath = normalizePath(info.sFilePath);
    data.sEntryPath = sPath;
    const int nSlash = sPath.lastIndexOf('/');
    if (nSlash >= 0)
    {
        QString sDir = sPath.left(nSlash);
        if (sDir.isEmpty())
            sDir = QStringLiteral("/");   // 只有 POSIX 根会出现（Windows 路径至少带盘符）
        else if (sDir.size() == 2 && sDir.at(1) == ':')
            sDir += QLatin1Char('/');     // "E:" -> "E:/"
        data.sLocation = sDir;
    }

    data.sArchivePath = normalizePath(m_fileManager->archivePath());
    data.nArchiveVersion = m_fileManager->archiveVersion();
    data.bPending = m_fileManager->isPendingEntry(info.sFilePath);
    data.bHasFsMetadata = info.bHasFsMetadata;
    data.nCreatedTime   = info.nCreatedTime;
    data.nModifiedTime  = info.nModifiedTime;
    data.nAccessedTime  = info.nAccessedTime;
    data.nAttributes    = info.nAttributes;
    data.listTags       = info.listTags;
    quint32 nCrc = 0;
    data.bHasCrc = m_fileManager->contentCrcFor(info.sFilePath, &nCrc);
    if (data.bHasCrc)
        data.nCrc = nCrc;
    return data;
}

void MainWindow::showFileProperty(const FILEINFO &info)
{
    const DlgFileProperty::Data data = filePropertyData(info);
    DlgFileProperty dlg(data, this);
    dlg.exec();
}

// 「属性」：树形列表里的目录节点（归档里没有真实目录条目，属性按当前可见子树汇总）
DlgFileProperty::Data MainWindow::folderPropertyData(const QModelIndex &index) const
{
    DlgFileProperty::Data data;
    if (m_hModelFilePath == nullptr)
        return data;

    const QModelIndex itemIndex = index.siblingAtColumn(0);
    QStandardItem *pItem = m_hModelFilePath->itemFromIndex(itemIndex);
    if (pItem == nullptr || !pItem->data(FileListModel::IsDirRole).toBool())
        return data;   // 菜单打开期间列表被重建/换了内容：宁可不开，也不显示错的数据

    data.bDir = true;
    data.sName = pItem->text();
    data.sType = tr("文件夹");
    data.bFiltered = !m_sFilter.isEmpty();

    // 大小与列表"大小"列同源（子树合计）；缺失时才现算
    const QModelIndex sizeIndex = itemIndex.siblingAtColumn(1);
    const QVariant vBytes = sizeIndex.data(FileListModel::BytesRole);
    data.nSize = vBytes.isValid() ? vBytes.toLongLong() : folderBytesOfItem(pItem);
    countFolderItems(pItem, data.nFileCount, data.nDirCount);

    data.sEntryPath = folderPathOfItem(pItem);
    if (pItem->parent() != nullptr)
        data.sLocation = folderPathOfItem(pItem->parent());

    data.sArchivePath = normalizePath(m_fileManager->archivePath());
    data.nArchiveVersion = m_fileManager->archiveVersion();
    return data;
}

void MainWindow::showFolderProperty(const QModelIndex &index)
{
    const DlgFileProperty::Data data = folderPropertyData(index);
    if (data.sName.isEmpty())
        return;
    DlgFileProperty dlg(data, this);
    dlg.exec();
}

// 标签栏“+”按钮：新建空白标签页
void MainWindow::onActionNewTabTriggered()
{
    newEmptyTab();
}

void MainWindow::on_treeView_doubleClicked(const QModelIndex &index)
{
    handleFileListDoubleClick(index);
}

// 文件列表双击的统一处理：
//   目录节点 -> 展开 / 收起该节点（不打开文件）
//   文件节点 -> 在当前激活的标签页打开该文件
void MainWindow::handleFileListDoubleClick(const QModelIndex &index)
{
    if (!index.isValid())
        return;

    // 双击可能落在"大小/类型"列：统一按第 0 列的业务项判断/取 FILEINFO
    const QModelIndex itemIndex = index.siblingAtColumn(0);
    QStandardItem *pItem = m_hModelFilePath->itemFromIndex(itemIndex);
    // 目录节点：双击仅做展开 / 折叠，不打开文件（缩略图模式只展示文件，不会走到这里）
    if (!isFileLeafItem(pItem))
    {
        if (m_eViewMode == ViewTree)
            m_tvFilePath->setExpanded(itemIndex, !m_tvFilePath->isExpanded(itemIndex));
        return;
    }

    // 双击：在当前激活的标签页打开选中的记录项文件
    FILEINFO fInfo = pItem->data(FileListModel::InfoRole).value<FILEINFO>();
    openFileInCurrentTab(fInfo);
}

// 事件过滤器：在文件列表 viewport 上直接接管「鼠标双击」。
//
// 为什么不能只依赖 doubleClicked 信号：
// 1) QAbstractItemView 只有在按下时该行「已经是 currentIndex」才会把双击
//    派发成 doubleClicked。第一次双击一个尚未选中的行会被静默丢弃 ——
//    表现为「双击文件夹毫无反应」，其实第一次双击只是把它选中了。
// 2) QTreeView 自带的 expandsOnDoubleClick（默认为 true）会在业务侧
//    setExpanded() 之后再次展开/折叠该节点，两者互相抵消 ——
//    表现为「双击文件夹展开后立刻又合上」。
// 在 viewport 层接管双击可同时绕开这两个问题，且对目录与文件一视同仁。
bool MainWindow::eventFilter(QObject *obj, QEvent *event)
{
    // 标签栏：中键关闭「+」按钮随标签变化的重新定位
    if (ui->tabWidget != nullptr
        && (obj == ui->tabWidget->tabBar() || obj == ui->tabWidget))
    {
        if (obj == ui->tabWidget->tabBar()
            && event->type() == QEvent::MouseButtonRelease
            && static_cast<QMouseEvent *>(event)->button() == Qt::MiddleButton)
        {
            const int nIndex = ui->tabWidget->tabBar()->tabAt(
                static_cast<QMouseEvent *>(event)->pos());
            if (nIndex >= 0)
            {
                onTabCloseRequested(nIndex);
                return true;
            }
        }
        else if (event->type() == QEvent::Resize || event->type() == QEvent::LayoutRequest
                 || event->type() == QEvent::Paint)
        {
            // 布局/绘制完成后再定位（直接改几何会被本次布局覆盖）。
            // Paint 也挂上：标签增删不一定发 LayoutRequest，而绘制一定在布局之后。
            if (!m_bNewTabBtnPending)
            {
                m_bNewTabBtnPending = true;
                QTimer::singleShot(0, this, [this]() {
                    m_bNewTabBtnPending = false;
                    updateNewTabButton();
                });
            }
        }
        else if (event->type() == QEvent::Show)
        {
            QTimer::singleShot(0, this, [this]() { updateNewTabButton(); });
        }
    }

    // 树形模式下点列头排序会 takeRow/appendRow 重建行，导致已展开目录被折叠：
    // 按下时记住展开状态，松开后（排序已执行）恢复。
    // 注意：表头鼠标事件落在 header 的 viewport 上，过滤器装在那里才收得到。
    QWidget *pHeaderViewport = m_tvFilePath ? m_tvFilePath->header()->viewport() : nullptr;
    if (pHeaderViewport != nullptr && obj == pHeaderViewport)
    {
        if (event->type() == QEvent::MouseButtonPress)
        {
            m_setExpandedBeforeSort.clear();
            if (m_eViewMode == ViewTree)
            {
                for (int i = 0; i < m_hModelFilePath->rowCount(); ++i)
                    collectExpandedItems(m_hModelFilePath->index(i, 0), QString(),
                                         m_setExpandedBeforeSort);
            }
            m_sSelectedPathBeforeSort.clear();
            const QModelIndex cur = m_tvFilePath->currentIndex();
            if (cur.isValid())
            {
                if (QStandardItem *it = m_hModelFilePath->itemFromIndex(cur))
                {
                    const QVariant v = it->data(FileListModel::InfoRole);
                    if (v.canConvert<FILEINFO>())
                        m_sSelectedPathBeforeSort = v.value<FILEINFO>().sFilePath;
                }
            }
        }
        else if (event->type() == QEvent::MouseButtonRelease)
        {
            QTimer::singleShot(0, this, [this]() {
                if (m_eViewMode == ViewTree && !m_setExpandedBeforeSort.isEmpty())
                {
                    for (int i = 0; i < m_hModelFilePath->rowCount(); ++i)
                        restoreExpandedItems(m_hModelFilePath->index(i, 0), QString(),
                                             m_setExpandedBeforeSort);
                }
                m_setExpandedBeforeSort.clear();
                if (!m_sSelectedPathBeforeSort.isEmpty())
                {
                    std::function<QModelIndex(const QModelIndex &)> findItem =
                        [&](const QModelIndex &parent) -> QModelIndex {
                        for (int r = 0; r < m_hModelFilePath->rowCount(parent); ++r)
                        {
                            const QModelIndex idx = m_hModelFilePath->index(r, 0, parent);
                            if (QStandardItem *it = m_hModelFilePath->itemFromIndex(idx))
                            {
                                const QVariant v = it->data(FileListModel::InfoRole);
                                if (v.canConvert<FILEINFO>()
                                    && v.value<FILEINFO>().sFilePath == m_sSelectedPathBeforeSort)
                                    return idx;
                            }
                            const QModelIndex child = findItem(idx);
                            if (child.isValid()) return child;
                        }
                        return QModelIndex();
                    };
                    const QModelIndex hit = findItem(QModelIndex());
                    if (hit.isValid())
                        m_tvFilePath->setCurrentIndex(hit);
                    m_sSelectedPathBeforeSort.clear();
                }
            });
        }
        // 不消费事件：让 QHeaderView/QTreeView 正常完成排序
    }

    if (m_hModelFilePath
        && ((m_tvFilePath && obj == m_tvFilePath->viewport())
            || (m_lvFilePath && obj == m_lvFilePath->viewport()))
        && event->type() == QEvent::MouseButtonDblClick)
    {
        QAbstractItemView *pView = (m_lvFilePath && obj == m_lvFilePath->viewport())
                                       ? static_cast<QAbstractItemView *>(m_lvFilePath)
                                       : static_cast<QAbstractItemView *>(m_tvFilePath);
        QMouseEvent *me = static_cast<QMouseEvent *>(event);
        const QModelIndex index = pView->indexAt(me->position().toPoint());
        if (index.isValid())
        {
            handleFileListDoubleClick(index);
            return true;   // 已处理，不再走视图默认的双击逻辑
        }
    }
    return QMainWindow::eventFilter(obj, event);
}

bool MainWindow::confirmDiscardOrSaveIfDirty()
{
    if (!m_fileManager->isDirty())
        return true;

    QMessageBox::StandardButton btn = QMessageBox::warning(this,
        tr("未保存的改动"),
        tr("当前归档存在未保存的改动，继续操作将丢失这些改动。\n是否先保存？"),
        QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel,
        QMessageBox::Save);

    if (btn == QMessageBox::Cancel)
        return false;

    if (btn == QMessageBox::Save)
    {
        // 后台 CRC 仍持有归档句柄时 rename 会失败：先等它结束（超时则明确提示）。
        if (!waitForMappedCrc(15000))
        {
            QMessageBox::warning(this, tr("保存失败"),
                tr("后台仍在校验归档内容，暂时无法保存。请稍等几秒后重试。"));
            return false;
        }
        // 保存会重写归档：先释放内存映射并停止播放（Windows 下映射中的文件不能改名）
        if (!m_mapMappedInputs.isEmpty())
        {
            stopCurrentPlaybackAndInvalidate();
            releaseAllMappedInputs();
        }
        // P0-3：仍有停播未确认的实例时，映射不能释放，归档也无法安全替换。
        // 先做一次非阻塞复查：后台线程可能刚好在等待期间退出了。
        pollStopUnsafeInstances();
        if (!m_setStopUnsafe.isEmpty())
        {
            QMessageBox::warning(this, tr("保存失败"),
                tr("有插件的后台播放未能确认停止，其内存映射仍在使用。\n"
                   "此时替换归档可能失败或产生不一致，已取消本次操作。\n"
                   "程序会每秒自动复查；也可关闭正在播放的标签页后重试。"));
            return false;
        }
        if (!prepareThumbnailsForArchiveWrite(5000))
        {
            QMessageBox::warning(this, tr("保存失败"),
                tr("缩略图仍在读取归档内容，暂时无法替换文件。请稍等几秒后重试。"));
            refreshFilePathView();   // 恢复待生成的缩略图
            return false;
        }
        QApplication::setOverrideCursor(Qt::WaitCursor);
        bool bOk = m_fileManager->save();
        QApplication::restoreOverrideCursor();
        if (!bOk)
        {
            const QString sReason = m_fileManager->lastError();
            refreshFilePathView();   // 保存前取消了缩略图任务，失败后恢复
            QMessageBox::critical(this, tr("保存失败"),
                tr("保存失败，操作已取消。改动仍保留在内存中，可以重试。\n\n原因：%1")
                   .arg(sReason.isEmpty() ? tr("未知错误") : sReason));
            return false;
        }
        // 这里也必须完成与 Ctrl+S 相同的刷新：用户可能在“加载文件”提示里选择保存，
        // 随后取消文件对话框；若列表仍持有保存前的路径/偏移快照，接下来双击、
        // 导出、属性都会报“归档内未找到文件”，大文件还会拿 nContentPositon=0 去映射。
        afterArchiveSaved();
    }
    // Discard 或 保存成功：继续
    return true;
}

void MainWindow::afterArchiveSaved()
{
    // 保存会重写归档并重算内容偏移/展示路径；列表、标签页快照和缩略图都必须作废。
    invalidateThumbnailCache();
    refreshTabInfosFromManager();
    refreshFilePathView();
    syncActionStates();
}

void MainWindow::on_actionLoadFile_triggered()
{
    if (m_fileManager->isBusy())
    {
        QMessageBox::information(this, tr("操作进行中"),
            tr("正在保存/合并归档，请等待完成后再执行其它文件操作。"));
        return;
    }

    if (!confirmDiscardOrSaveIfDirty())
        return;

    QString sFileName = QFileDialog::getOpenFileName(this, tr("加载文件"), "", FwArc::fileDialogFilter());
    if(sFileName.isEmpty())
        return;

    // 已加载归档且开着标签页时先确认：加载会关闭所有标签页（只影响查看，不影响归档内容）
    if (m_fileManager->isArchiveLoaded() && ui->tabWidget->count() > 0)
    {
        const auto btn = QMessageBox::question(this, tr("加载文件"),
            tr("加载新归档会关闭当前所有标签页。\n是否继续？"),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (btn != QMessageBox::Yes)
            return;
    }

    // —— P1-6：先做完整解析校验，失败时完全不触碰当前归档 / 视图 / 脏状态 ——
    loadArchiveFile(sFileName);
}

void MainWindow::loadArchiveFile(const QString &sFileName)
{
    // P1-9：校验与加载共用同一次解析结果（旧实现各 parse 一次头部+条目区）
    FwArc::Meta preParsed;
    QString sErr;
    if (!m_fileManager->validateMergedFile(sFileName, &preParsed, &sErr))
    {
        QMessageBox::critical(this, QStringLiteral("加载失败"),
            QStringLiteral("不是有效的归档文件或文件已损坏。\n%1\n\n原有归档与未保存的改动保持不变。").arg(sErr));
        return;
    }

    if (!m_fileManager->QLoadMergedFile(sFileName, &preParsed))
    {
        QMessageBox::critical(this, QStringLiteral("加载失败"),
            QStringLiteral("归档解析失败，原有归档保持不变。\n%1")
               .arg(m_fileManager->lastError()));
        syncActionStates();
        return;
    }
    if (m_fileManager->getFileInfos().isEmpty())
    {
        QMessageBox::critical(this, QStringLiteral("加载失败"),
            QStringLiteral("归档中没有可显示的条目，原有归档保持不变。"));
        syncActionStates();
        return;
    }

    m_hModelFilePath->setHeaderData(0, Qt::Horizontal, tr("文件路径"));
    updateWindowTitle();
    m_tvFilePath->header()->setVisible(true);

    // 换归档时清掉上一份的筛选词，避免新归档"看起来是空的"
    if (m_leFilter && !m_leFilter->text().isEmpty())
    {
        QSignalBlocker blocker(m_leFilter);
        m_leFilter->clear();
        m_sFilter.clear();
    }

    // 加载成功后重置所有已打开的文件标签页（旧归档的文件路径已失效）
    closeAllTabs();

    // 换归档：上一份的树展开状态与路径同名的目录无关，不能套到新归档上
    m_setExpandedTree.clear();
    m_bResetTreeExpansion = true;

    // 新归档的条目内容与上一份无关，缩略图缓存必须整体作废
    invalidateThumbnailCache();

    populateFilePathView(m_bShowPath);
    syncActionStates();
}

void MainWindow::on_actionMergeFile_triggered()
{
    if (m_fileManager->isBusy())
    {
        QMessageBox::information(this, tr("操作进行中"),
            tr("正在保存/合并归档，请等待完成后再执行其它文件操作。"));
        return;
    }
    if (m_bScanningFiles)
        return;   // 上一次合并的目录扫描尚未结束

    DlgMergeFile dlgMergeFile(this);
    if(dlgMergeFile.exec() == QDialog::Accepted)
    {
        QStringList qLFilePaths = dlgMergeFile.getFilePaths();
        QString sMergedFilePath = dlgMergeFile.getMergedFilePath();

        // 先显示进度框，再展开目录：大目录展开可能耗时，不能等展开完才弹框。
        QProgressDialog dlgProgress(QStringLiteral("正在扫描待合并文件..."), QStringLiteral("取消"), 0, 0, this);
        dlgProgress.setWindowTitle(QStringLiteral("合并文件"));
        dlgProgress.setWindowModality(Qt::WindowModal);
        dlgProgress.setMinimumDuration(0);
        dlgProgress.setAutoClose(false);
        dlgProgress.setAutoReset(false);
        bool bOk = false;
        QString sResultMsg;
        beginProgressOperation(&dlgProgress, &bOk, &sResultMsg);
        dlgProgress.show();
        QApplication::processEvents();
        setOperationsEnabled(false);

        // 扫描期间会周期性泵事件（FindFile），此时 FileManager 尚未 busy，
        // 用独立标志挡住 closeEvent / 重入，避免"窗口关了还继续合并"。
        m_bScanningFiles = true;
        QVector<QString> vtFilesPaths;
        foreach(QString sFilePath, qLFilePaths)
        {
            QFileInfo fileinfo(sFilePath);
            if(fileinfo.isDir())
            {
                QStringList arrFiles;
                FindFile(sFilePath, arrFiles, true);
                foreach(QString sSubFilePath, arrFiles)
                {
                    vtFilesPaths.push_back(sSubFilePath);
                }
            }
            else if(fileinfo.isFile())
            {
                vtFilesPaths.push_back(sFilePath);
            }
        }
        m_bScanningFiles = false;

        if (m_fileManager->isCancelRequested())
        {
            dlgProgress.close();
            endProgressOperation();
            setOperationsEnabled(true);
            syncActionStates();
            QMessageBox::information(this, QStringLiteral("合并文件"), QStringLiteral("已取消。"));
            return;
        }
        dlgProgress.setRange(0, 100);
        dlgProgress.setLabelText(QStringLiteral("正在合并文件..."));

        // 选中的文件里如果有已合并的 .dat：合并时会把它们拆分成条目，而不是当成普通文件
        int nArchivedInputs = 0;
        for (const QString &sP : vtFilesPaths)
        {
            QFile fProbe(sP);
            if (!fProbe.open(QIODevice::ReadOnly)) continue;
            char magic[4] = { 0, 0, 0, 0 };
            if (fProbe.read(magic, 4) == 4
                && std::memcmp(magic, FwArc::MAGIC, 4) == 0)
                ++nArchivedInputs;
        }
        if (nArchivedInputs > 0)
            dlgProgress.setLabelText(QStringLiteral(
                "正在合并文件（其中 %1 个是已合并归档，将先拆分再合并）...")
                    .arg(nArchivedInputs));

        emit QMergeFiles(vtFilesPaths, sMergedFilePath);

        dlgProgress.close();
        endProgressOperation();
        setOperationsEnabled(true);
        syncActionStates();

        if (!bOk)
        {
            QMessageBox::critical(this, QStringLiteral("合并文件"), sResultMsg);
        }
        else
        {
            // 合并成功：提供「打开合并结果」，省掉再走一次「加载文件」并重新选路径
            QMessageBox box(QMessageBox::Information, QStringLiteral("合并文件"),
                            sResultMsg, QMessageBox::NoButton, this);
            QPushButton *pOpen = box.addButton(tr("打开合并结果"), QMessageBox::AcceptRole);
            box.addButton(tr("关闭"), QMessageBox::RejectRole);
            box.setDefaultButton(pOpen);
            box.exec();
            if (box.clickedButton() == pOpen && confirmDiscardOrSaveIfDirty())
                loadArchiveFile(sMergedFilePath);
        }
    }
}

bool MainWindow::FindFile(const QString sDir, QStringList& arrFiles, bool bSubDir)
{
    QDir dir(sDir);
    if (!dir.exists())
        return false;

    // 空目录（或 exists 真但枚举失败）不能先 list.at(0)；
    // 同时用 canonical 路径 + 深度上限防止 junction/symlink 自指导致无限递归。
    QSet<QString> visited;
    // 扫描在 UI 线程同步执行：必须定期泵事件，否则「取消」按钮的点击只能排队，
    // 大目录扫描期间整个窗口无响应。计数跨递归共享，每 512 个条目处理一次。
    int nVisited = 0;
    std::function<void(const QString &, int)> walk = [&](const QString &sCurrent, int nDepth) {
        if (nDepth > 64 || m_fileManager->isCancelRequested())
            return;
        QDir d(sCurrent);
        if (!d.exists())
            return;
        QString sKey = QFileInfo(sCurrent).canonicalFilePath();
        if (sKey.isEmpty())
            sKey = d.absolutePath();
        sKey = sKey.toLower();
        if (visited.contains(sKey))
            return;
        visited.insert(sKey);

        d.setFilter(QDir::Dirs | QDir::Files);
        d.setSorting(QDir::DirsFirst);
        const QFileInfoList list = d.entryInfoList();
        for (const QFileInfo &fi : list)
        {
            if (m_fileManager->isCancelRequested())
                return;
            if ((++nVisited & 0x1FF) == 0)
            {
                QApplication::processEvents();
                if (m_fileManager->isCancelRequested())
                    return;
            }
            if (!fi.isDir())
            {
                arrFiles << d.absoluteFilePath(fi.fileName());
                continue;
            }
            if (!bSubDir)
                continue;
            if (fi.fileName() == QLatin1String(".") || fi.fileName() == QLatin1String(".."))
                continue;
            walk(fi.filePath(), nDepth + 1);
        }
    };
    walk(sDir, 0);
    return true;
}

QString MainWindow::choosePlugin(QString sFileExt)
{
    // 后缀层：完全由插件元数据 JSON 里的 "extensions" 决定（C1）。
    // 这里以前还有「用户映射」优先级和 6 张写死表：映射会按后缀永久覆盖自动判断，
    // 已删除；"内容优先"的完整路由见 decideRoute()。
    return PluginManager::getInstance()->pluginForExtension(sFileExt);
}

// 打开一个文件时的完整路由：读文件头，交给纯函数 decideRoute() 得结论。
MainWindow::RouteDecision MainWindow::routeForFile(const FILEINFO &info)
{
    const QString sByExt = choosePlugin(QFileInfo(info.sFilePath).suffix());
    // 只读 256 KB 文件头：OOXML/ODF 的关键标志都在开头，不必解析整个 ZIP。
    // 归档内条目与新增条目都走 entryHead；读不到时 head 为空，decideRoute 自然退回后缀。
    const QByteArray head = m_fileManager->entryHead(info.sFilePath, 256 * 1024);
    return decideRoute(info.sFileName, head, sByExt);
}

// 自动路由的纯函数部分（不碰文件系统，便于单测）：
//   1) 内容识别的结论能落到一个已安装插件时，以内容为准 —— 后缀被改过、没有后缀、
//      下载成 .dat/.bin 的文件都能开对；
//   2) 内容判断不出来，或只是 zip/ole2/xml 这类"容器/通用"格式时，按插件声明的后缀；
//   3) 两者都没有 = 没有插件能处理，由调用方弹「无法打开」对话框。
// 为什么排除这些弱格式：svg/fb2 以 <?xml 开头、修补过的 docx 头里没有
// word/document.xml —— 它们会被通用容器结论误抢，此时后缀声明更可靠。
// 例外：后缀层也没有结论时（无后缀的 .dat），弱格式结论仍是唯一线索，照用。
MainWindow::RouteDecision MainWindow::decideRoute(const QString &sFileName,
                                                  const QByteArray &head,
                                                  const QString &sByExtPath)
{
    RouteDecision d;
    d.detection = FormatResolver::detect(sFileName, head);

    const QString &sId = d.detection.formatId;
    const bool bWeakFormat = (sId == QStringLiteral("zip") || sId == QStringLiteral("ole2")
                              || sId == QStringLiteral("xml"));

    // 只有"内容明确给出的结论"才有资格推翻后缀：byExtension 的结论是循环论证。
    const bool bContentUsable = d.detection.fromContent && !d.detection.pluginExt.isEmpty()
                                && (!bWeakFormat || sByExtPath.isEmpty());

    if (bContentUsable)
    {
        const QString sByContent =
            PluginManager::getInstance()->pluginForExtension(d.detection.pluginExt);
        if (!sByContent.isEmpty())
        {
            d.pluginPath = sByContent;
            d.formatName = d.detection.displayName;
            d.basis = d.detection.detail;
            d.fromContent = true;
            d.contentOverride = (sByExtPath.isEmpty() || sByExtPath != sByContent);
            // 只记录"内容推翻了后缀"的少数情况：正常后缀命中不刷日志
            if (d.contentOverride)
                qDebug() << "[Plugin] 内容优先路由:" << sFileName << "->" << d.detection.formatId
                         << "(" << d.detection.detail << ")";
            return d;
        }
    }

    if (!sByExtPath.isEmpty())
    {
        d.pluginPath = sByExtPath;
        const QString sExt = QFileInfo(sFileName).suffix().toLower();
        d.basis = sExt.isEmpty() ? QStringLiteral("按插件声明的后缀")
                                 : QStringLiteral("按扩展名 .%1").arg(sExt);
    }
    return d;
}

void MainWindow::on_actionSplitFile_triggered()
{
    if (m_fileManager->isDirty()
        && QMessageBox::question(this, tr("未保存的改动"),
               tr("当前归档有未保存改动，分割将基于磁盘上的旧归档执行，"
                  "新添加的条目不会出现在结果中。\n是否继续？"),
               QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes)
        return;

    DlgSplitFile dlgSplitFile(this);
    // 已加载归档时预填"待分割文件路径"，用户不必再手选一次
    if (m_fileManager->isArchiveLoaded())
        dlgSplitFile.setMergedFilePath(m_fileManager->archivePath());
    if(dlgSplitFile.exec() == QDialog::Accepted)
    {
        const QString sMergedFilePath = dlgSplitFile.getMergedFilePath();

        // —— P2-3：完整解析 + 逐条合法性校验，与加载使用同一套规则 ——
        FwArc::Meta meta;
        QString sErr;
        if (!FileManager::inspectArchive(sMergedFilePath, meta, sErr))
        {
            QMessageBox::critical(this, QStringLiteral("分割失败"),
                QStringLiteral("不是有效的归档文件或文件已损坏。\n%1").arg(sErr));
            return;
        }
        const bool bIsSaveAsOldPath = dlgSplitFile.getIsSaveAsOldPath();
        QString sSplitFileDir;
        if (!bIsSaveAsOldPath)
            sSplitFileDir = dlgSplitFile.getSplitFileDir();

        // —— P0-2：按原路径写入是危险操作，必须二次确认 ——
        if (bIsSaveAsOldPath)
        {
            int nAbs = 0;
            qint64 nTotal = 0;
            for (const FwArc::Entry &e : meta.entries)
            {
                if (FwArc::isAbsolutePath(e.displayPath)) ++nAbs;
                nTotal += qint64(e.size);
            }
            const QString sWarn = QStringLiteral(
                "将按归档内记录的原始路径把 %1 个文件（共 %2）写回磁盘。\n"
                "其中 %3 个是绝对路径；换机器、换盘符或目录被复用时，可能写到完全无关的位置。\n\n"
                "已存在的目标文件不会被覆盖，程序会自动重命名。是否继续？")
                .arg(meta.entries.size()).arg(FwArc::formatSize(nTotal)).arg(nAbs);
            if (QMessageBox::warning(this, QStringLiteral("按原路径分割"), sWarn,
                                     QMessageBox::Yes | QMessageBox::No,
                                     QMessageBox::No) != QMessageBox::Yes)
                return;
        }

        // —— P1-4：进度 / 取消 / 完成汇总 ——
        QProgressDialog dlgProgress(QStringLiteral("正在分割文件..."), QStringLiteral("取消"), 0, 100, this);
        dlgProgress.setWindowTitle(QStringLiteral("分割文件"));
        dlgProgress.setWindowModality(Qt::WindowModal);
        dlgProgress.setMinimumDuration(0);
        dlgProgress.setAutoClose(false);
        dlgProgress.setAutoReset(false);
        dlgProgress.setValue(0);

        bool bOk = false;
        QString sResultMsg;
        beginProgressOperation(&dlgProgress, &bOk, &sResultMsg);
        dlgProgress.show();
        QApplication::processEvents();
        setOperationsEnabled(false);

        emit QSplitFiles(sMergedFilePath, bIsSaveAsOldPath, sSplitFileDir);

        dlgProgress.close();
        endProgressOperation();
        setOperationsEnabled(true);
        syncActionStates();

        if (sResultMsg.isEmpty()) sResultMsg = QStringLiteral("分割未返回结果（归档可能已损坏）");
        if (bOk) QMessageBox::information(this, QStringLiteral("分割文件"), sResultMsg);
        else     QMessageBox::warning(this, QStringLiteral("分割文件"), sResultMsg);
    }
}

// 按当前模式刷新列表：树形（默认）/ 平铺 / 缩略图
void MainWindow::populateFilePathView(bool bShowPath)
{
    m_bShowPath = bShowPath;

    // 三种模式都支持排序：
    //   * 平铺/缩略图：全局排序；
    //   * 树形：同级排序、目录优先（FileListModel 实现），不会把层级拍平。
    // 排序在初始化时就已启用，这里不再反复 setSortingEnabled(true)：
    // Qt 每次调用都会立即 sort 当前模型，若发生在捕获展开状态之前，会把
    // 已展开目录折叠后又被当成"本来就没展开"。
    if (m_eViewMode == ViewTree)
    {
        populateFilePathTree();   // 内部捕获展开状态 -> 重建/排序 -> 恢复
    }
    else
    {
        // 离开缩略图后，尚未生成的空闲回调立即作废（不用等它们自己检查模式）
        cancelThumbnailGeneration();
        if (m_eViewMode == ViewThumbnail)
        {
            // 缩略图自己负责排序并在排序后重建索引映射；这里不能再排一次，
            // 否则第二次 takeRow/appendRow 会把刚建好的持久索引再次打散。
            populateFilePathThumbnails(bShowPath);
        }
        else
        {
            populateFilePathList(bShowPath);
            applyCurrentSortToModel();
        }
    }

    // 树控件承载树形/平铺，缩略图列表单独一格
    if (m_swFilePathViews != nullptr)
    {
        m_swFilePathViews->setCurrentWidget(m_eViewMode == ViewThumbnail
                                                ? static_cast<QWidget *>(m_lvFilePath)
                                                : static_cast<QWidget *>(m_tvFilePath));
    }

    updateViewActionState();

    // 模型重建后「大小/类型」两列的内容宽度会变，路径列宽度跟着重算。
    // 缩略图模式没有表头，不做这项（也避免在隐藏视图上反复触发）。
    if (m_eViewMode != ViewThumbnail)
        QTimer::singleShot(0, this, [this]() { adjustPathColumnWidth(); });
}

// 「文件路径」列宽度 = 视口宽 - 大小/类型两列，且不低于 kMinPathColumnWidth。
// 面板拖窄到放不下时保留最小宽度，由横向滚动条兜底，而不是把文件名压成一条缝。
void MainWindow::adjustPathColumnWidth()
{
    if (m_tvFilePath == nullptr || m_eViewMode == ViewThumbnail)
        return;
    QHeaderView *pHeader = m_tvFilePath->header();
    if (pHeader == nullptr)
        return;

    const int nOther = pHeader->sectionSize(1) + pHeader->sectionSize(2)
                     + pHeader->sectionSize(3);
    const int nAvail = m_tvFilePath->viewport()->width();
    const int nWant = qMax(kMinPathColumnWidth, nAvail - nOther);
    if (nWant <= 0 || pHeader->sectionSize(0) == nWant)
        return;

    m_bAdjustingPathColumn = true;
    pHeader->resizeSection(0, nWant);
    m_bAdjustingPathColumn = false;
}

// 按表头当前的排序列/方向排序（树形模式下为同级排序、目录优先）
void MainWindow::applyCurrentSortToModel()
{
    if (m_tvFilePath == nullptr || m_hModelFilePath == nullptr)
        return;
    QHeaderView *pHeader = m_tvFilePath->header();
    const int nSection = pHeader->sortIndicatorSection();
    m_hModelFilePath->sort(nSection >= 0 ? nSection : 0,
                           nSection >= 0 ? pHeader->sortIndicatorOrder()
                                         : Qt::AscendingOrder);
}

// 平铺模式：每个文件一行，文本为完整路径或文件名
void MainWindow::populateFilePathList(bool bShowPath)
{
    QVector<FILEINFO> vtFileInfos = m_fileManager->getFileInfos();
    int nRow = 0;
    m_hModelFilePath->removeRows(0, m_hModelFilePath->rowCount());
    for (const FILEINFO &info : vtFileInfos)
    {
        if (!matchesFilter(info))
            continue;
        QStandardItem* hItemName = new QStandardItem(bShowPath ? info.sFilePath : info.sFileName);
        hItemName->setToolTip(normalizePath(info.sFilePath));
        QVariant var;
        var.setValue(info);
        hItemName->setData(var, FileListModel::InfoRole);
        QStandardItem *pSize = new QStandardItem(FwArc::formatSize(info.nFileLen));
        pSize->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
        pSize->setData(info.nFileLen, Qt::UserRole);   // 大小列按数值排序用
        QStandardItem *pType = new QStandardItem(typeLabel(info.sFileName));
        QStandardItem *pTags = new QStandardItem(tagsColumnText(info));
        if (!info.listTags.isEmpty())
            pTags->setToolTip(tr("标记：%1").arg(tagsColumnText(info)));
        m_hModelFilePath->setItem(nRow, 0, hItemName);
        m_hModelFilePath->setItem(nRow, 1, pSize);
        m_hModelFilePath->setItem(nRow, 2, pType);
        m_hModelFilePath->setItem(nRow, 3, pTags);
        ++nRow;
    }
}

// 缩略图模式：每个文件一格。非图片与暂未解码的图片先显示类型角标，
// 图片随后由 processNextThumbnail() 逐张替换成内容缩略图。
void MainWindow::populateFilePathThumbnails(bool bShowPath)
{
    cancelThumbnailGeneration();   // 作废上一轮（筛选/增删导致重建）的空闲回调
    // 已缓存的图标也占模型内存：把它们计入本轮预算，避免“1024 缓存 + 1024 新生成”
    // 在反复切换视图时叠成两倍。
    m_nThumbnailGenerated = m_mapThumbnailIcons.size();
    m_hModelFilePath->removeRows(0, m_hModelFilePath->rowCount());

    const QVector<FILEINFO> vtFileInfos = m_fileManager->getFileInfos();
    int nRow = 0;
    for (const FILEINFO &info : vtFileInfos)
    {
        if (!matchesFilter(info))
            continue;

        auto *pItem = new QStandardItem(bShowPath ? normalizePath(info.sFilePath)
                                                  : info.sFileName);
        QString sTip = tr("%1\n%2 · %3")
                           .arg(normalizePath(info.sFilePath),
                                FwArc::formatSize(info.nFileLen),
                                typeLabel(info.sFileName));
        if (!info.listTags.isEmpty())
            sTip += tr("\n标记：%1").arg(tagsColumnText(info));
        pItem->setToolTip(sTip);
        QVariant var;
        var.setValue(info);
        pItem->setData(var, FileListModel::InfoRole);
        // 图标模式下文件名在图标下方，居中显示；路径/名称切换与平铺模式一致
        pItem->setTextAlignment(Qt::AlignHCenter | Qt::AlignVCenter);

        const auto itCached = m_mapThumbnailIcons.constFind(info.sFilePath);
        const bool bHasCachedThumb = (itCached != m_mapThumbnailIcons.constEnd());
        pItem->setIcon(bHasCachedThumb ? itCached.value()
                                       : tabIconForFile(info.sFileName));

        QStandardItem *pSize = new QStandardItem(FwArc::formatSize(info.nFileLen));
        pSize->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
        pSize->setData(info.nFileLen, Qt::UserRole);
        QStandardItem *pType = new QStandardItem(typeLabel(info.sFileName));
        QStandardItem *pTags = new QStandardItem(tagsColumnText(info));
        if (!info.listTags.isEmpty())
            pTags->setToolTip(tr("标记：%1").arg(tagsColumnText(info)));

        m_hModelFilePath->setItem(nRow, 0, pItem);
        m_hModelFilePath->setItem(nRow, 1, pSize);
        m_hModelFilePath->setItem(nRow, 2, pType);
        m_hModelFilePath->setItem(nRow, 3, pTags);

        if (!bHasCachedThumb && info.nFileLen > 0 && info.nFileLen <= kThumbnailMaxBytes
            && isThumbnailImageName(info.sFileName))
        {
            m_listThumbnailPending << info.sFilePath;
        }
        ++nRow;
    }

    applyCurrentSortToModel();

    // FileListModel::sort() 用 takeRow/appendRow 移动整行，排序前记录的
    // QPersistentModelIndex 会随「行被移除」而失效。必须排序后按当前行重建映射，
    // 否则空闲回调拿到的索引无效，界面上就只剩类型角标、永远等不到缩略图。
    for (int r = 0; r < m_hModelFilePath->rowCount(); ++r)
    {
        const QStandardItem *pRow = m_hModelFilePath->item(r, 0);
        if (pRow == nullptr || !isFileLeafItem(pRow))
            continue;
        const FILEINFO info = pRow->data(FileListModel::InfoRole).value<FILEINFO>();
        m_mapThumbnailIndex.insert(info.sFilePath,
                                   QPersistentModelIndex(m_hModelFilePath->index(r, 0)));
    }
    scheduleNextThumbnail();
}

// 归档里的条目能不能尝试解码成缩略图（按后缀先筛，避免对每个二进制文件都读一遍）
// 能否出缩略图：以 Qt 实际支持的图片格式为准（运行期由部署的 imageformats 插件决定），
// 不再维护第二份后缀表 —— 旧表和「图片插件声明的后缀」「实际部署的解码器」长期不一致
// （.tga/.icns/.wbmp 能打开却没有缩略图，.tif 有缩略图又打不开）。
bool MainWindow::isThumbnailImageName(const QString &sFileName)
{
    static const QSet<QString> kDecodableSuffixes = [] {
        QSet<QString> set;
        for (const QByteArray &fmt : QImageReader::supportedImageFormats())
            set.insert(QString::fromLatin1(fmt).toLower());
        return set;
    }();

    const QString sExt = QFileInfo(sFileName).suffix().toLower();
    if (sExt.isEmpty())
        return false;
    if (kDecodableSuffixes.contains(sExt))
        return true;

    // 别名后缀（.jpe/.jfif 等）不一定和 Qt 的格式名同名：插件把它归为图片时也尝试解码，
    // 解码失败会自然落回类型角标（renderEntryThumbnail 已有空图兜底）。
    return PluginManager::getInstance()->categoryForExtension(sExt) == QStringLiteral("image");
}

// 纯解码 + 合成，可在任意线程调用（只操作 QImage/QBuffer/QImageReader，不碰 QPixmap）。
// 返回 128×128 的成品底片；GUI 线程只负责 QPixmap::fromImage()。
static QImage renderThumbnailImage(const QByteArray &data)
{
    if (data.isEmpty())
        return QImage();

    QBuffer buffer;
    buffer.setData(data);
    if (!buffer.open(QIODevice::ReadOnly))
        return QImage();

    QImageReader reader(&buffer);
    // 归档条目来自外部文件，后缀不一定可信：以内容嗅探为准；
    // 并把 EXIF 方向应用上，否则手机竖拍的图在网格里会躺倒。
    reader.setDecideFormatFromContent(true);
    reader.setAutoTransform(true);

    // 解码前先看尺寸，避免超大图在内存里展开（解压炸弹/误改后缀）
    const QSize szSource = reader.size();
    if (szSource.isValid()
        && qint64(szSource.width()) * qint64(szSource.height()) > kThumbnailMaxPixels)
        return QImage();
    if (szSource.isValid())
    {
        const QSize szTarget = szSource.scaled(
            QSize(kThumbnailPixelSize, kThumbnailPixelSize), Qt::KeepAspectRatio);
        if (szTarget.isValid() && !szTarget.isEmpty())
            reader.setScaledSize(szTarget);   // 支持缩放的解码器不必先解出全尺寸
    }

    const QImage img = reader.read();
    if (img.isNull())
        return QImage();

    const QImage scaled = img.scaled(QSize(kThumbnailPixelSize, kThumbnailPixelSize),
                                     Qt::KeepAspectRatio, Qt::SmoothTransformation);
    if (scaled.isNull())
        return QImage();

    // 统一画进正方形底片：不同宽高比的图片都占满 96×96 的图标格，
    // 文件名才会在同一行对齐；否则宽图会把文字顶上去、竖图把文字压下去。
    QImage canvas(kThumbnailPixelSize, kThumbnailPixelSize, QImage::Format_ARGB32_Premultiplied);
    canvas.fill(Qt::transparent);
    {
        QPainter painter(&canvas);
        painter.setRenderHint(QPainter::Antialiasing, true);
        QPainterPath pathTile;
        pathTile.addRoundedRect(QRectF(0.5, 0.5, kThumbnailPixelSize - 1.0,
                                       kThumbnailPixelSize - 1.0),
                                12.0, 12.0);
        painter.fillPath(pathTile, QColor(QStringLiteral("#F7F9FC")));
        painter.setPen(QPen(QColor(QStringLiteral("#E3E8EF")), 1.0));
        painter.drawPath(pathTile);
        painter.setClipPath(pathTile);
        painter.drawImage(QPoint((kThumbnailPixelSize - scaled.width()) / 2,
                                 (kThumbnailPixelSize - scaled.height()) / 2),
                          scaled);
    }

    // 128px 源图直接用于 96px 网格：不声明 2x DPR，否则 Qt 会按 64 逻辑像素
    // 解释并放大，反而发糊；同时每张图的实际内存也小一半以上。
    return canvas;
}

// 后台任务只拿到“归档路径 + 偏移 + 长度”快照，自己开 QFile 读取；不引用
// FileManager（它的 QMap/QFile 游标属于 GUI 线程）。普通文件和未保存的新增
// 条目（pending）分别按源路径读取；归档条目在 GUI 线程先取到期望 CRC，读完校验。
struct ThumbnailJob
{
    QString  sKey;
    QString  sArchivePath;
    QString  sSourcePath;
    qint64   nPos = 0;
    qint64   nLen = 0;
    quint32  nExpectedCrc = 0;
    bool     bHasCrc = false;
    bool     bPending = false;
    quint64  nEpoch = 0;
};

static QByteArray readThumbnailJobData(const ThumbnailJob &job)
{
    if (job.nLen <= 0 || job.nLen > kThumbnailMaxBytes)
        return QByteArray();

    QFile f(job.bPending ? job.sSourcePath : job.sArchivePath);
    if (!f.open(QIODevice::ReadOnly))
        return QByteArray();

    if (job.bPending)
    {
        // 新增条目没有归档内偏移；源文件在等待期间被改写则放弃（保存前会再核对）。
        if (f.size() != job.nLen)
            return QByteArray();
    }
    else
    {
        if (job.nPos < 0 || job.nPos > f.size() || job.nLen > f.size() - job.nPos)
            return QByteArray();
        if (!f.seek(job.nPos))
            return QByteArray();
    }

    QByteArray data;
    try
    {
        data.resize(qsizetype(job.nLen));
    }
    catch (const std::bad_alloc &)
    {
        return QByteArray();
    }

    qint64 done = 0;
    while (done < job.nLen)
    {
        const qint64 n = qMin<qint64>(job.nLen - done, 1024 * 1024);
        const qint64 got = f.read(data.data() + done, n);
        if (got <= 0)
            return QByteArray();
        done += got;
    }

    if (job.bHasCrc)
    {
        FwArc::Crc32 crc;
        crc.update(data);
        if (crc.value() != job.nExpectedCrc)
            return QByteArray();
    }
    return data;
}

class ThumbnailDecodeTask : public QRunnable
{
public:
    using Done = std::function<void(const QString &, const QImage &, quint64)>;

    ThumbnailDecodeTask(ThumbnailJob job, QSharedPointer<ThumbnailGate> gate, Done done)
        : m_job(std::move(job)), m_gate(std::move(gate)), m_done(std::move(done))
    {
        setAutoDelete(true);
    }

    void run() override
    {
        // 运行期自检：这个任务一旦在 GUI 线程执行，就会重新引入卡顿，
        // 也为“QPixmap 不能在后台线程创建”之外的线程归属留下明确日志。
        if (QCoreApplication::instance() != nullptr
            && QThread::currentThread() == QCoreApplication::instance()->thread())
        {
            qWarning() << "[Thumb] 缩略图解码任务意外运行在 GUI 线程";
        }

        QImage img;
        try
        {
            if (m_gate
                && m_gate->generation.load(std::memory_order_relaxed) == m_job.nEpoch)
            {
                const QByteArray data = readThumbnailJobData(m_job);
                if (!data.isEmpty()
                    && m_gate->generation.load(std::memory_order_relaxed) == m_job.nEpoch)
                {
                    img = renderThumbnailImage(data);
                }
            }
        }
        catch (...)
        {
            img = QImage();   // 后台线程不能让异常逃逸，否则 std::terminate
        }

        // 共享计数在任务线程归还：保存前主线程阻塞等待它归零，不依赖事件循环。
        if (m_gate)
        {
            QMutexLocker locker(&m_gate->mutex);
            if (--m_gate->inFlight < 0)
                m_gate->inFlight = 0;
            if (m_gate->inFlight == 0)
                m_gate->idle.wakeAll();
        }

        if (!m_done)
            return;
        QCoreApplication *pApp = QCoreApplication::instance();
        if (pApp == nullptr)
            return;   // 应用正在退出：结果已无意义

        const Done done = m_done;
        const QString sPath = m_job.sKey;
        const quint64 nEpoch = m_job.nEpoch;
        QMetaObject::invokeMethod(pApp, [done, sPath, img, nEpoch]() {
            done(sPath, img, nEpoch);
        }, Qt::QueuedConnection);
    }

private:
    ThumbnailJob m_job;
    QSharedPointer<ThumbnailGate> m_gate;
    Done m_done;
};

// 读归档内容并解码成缩略图（与界面无关，测试可以直接调用）。
// 生产路径走 ThumbnailDecodeTask；这个同步版本保留给测试/调试。
QPixmap MainWindow::renderEntryThumbnail(const FILEINFO &info) const
{
    if (m_fileManager == nullptr || !m_fileManager->isArchiveLoaded())
        return QPixmap();
    if (!isThumbnailImageName(info.sFileName)
        || info.nFileLen <= 0 || info.nFileLen > kThumbnailMaxBytes)
        return QPixmap();

    QString sErr;
    const QByteArray data = m_fileManager->entryContent(info.sFilePath, &sErr);
    if (data.isEmpty())
        return QPixmap();
    return QPixmap::fromImage(renderThumbnailImage(data));
}

void MainWindow::cancelThumbnailGeneration()
{
    ++m_nThumbnailEpoch;             // 已排队的 singleShot/后台任务看到代数不同就直接退出
    if (m_thumbGate)
        m_thumbGate->generation.store(m_nThumbnailEpoch, std::memory_order_relaxed);
    m_listThumbnailPending.clear();
    m_mapThumbnailIndex.clear();
}

void MainWindow::invalidateThumbnailCache()
{
    cancelThumbnailGeneration();
    m_mapThumbnailIcons.clear();
}

void MainWindow::scheduleNextThumbnail()
{
    if (m_thumbPool == nullptr || m_listThumbnailPending.isEmpty()
        || m_eViewMode != ViewThumbnail)
        return;
    if (m_nThumbnailInFlight >= kThumbnailMaxInFlight)
        return;   // 由 onThumbnailDecoded 在任务回来后再续
    const quint64 nEpoch = m_nThumbnailEpoch;
    QTimer::singleShot(0, this, [this, nEpoch]() { processNextThumbnail(nEpoch); });
}

// 提交一张到后台线程池：GUI 只做“取任务快照 + 提交”，不读文件、不解码。
// 最多 kThumbnailMaxInFlight 张同时在途；每完成一张再由 onThumbnailDecoded 续下一张。
void MainWindow::processNextThumbnail(quint64 nEpoch)
{
    if (nEpoch != m_nThumbnailEpoch || m_eViewMode != ViewThumbnail)
        return;
    if (m_listThumbnailPending.isEmpty() || m_thumbPool == nullptr)
        return;
    if (m_nThumbnailInFlight >= kThumbnailMaxInFlight)
        return;
    if (m_nThumbnailGenerated >= kThumbnailMaxGenerated)
    {
        // 模型项会持有 QIcon，缓存上限并不能限制内存；达到本轮总量后立即停。
        m_listThumbnailPending.clear();
        statusBar()->showMessage(
            tr("图片较多，本次只为前 %1 张生成缩略图；可用筛选查看其余文件。")
                .arg(kThumbnailMaxGenerated), 8000);
        return;
    }

    const QString sPath = m_listThumbnailPending.takeFirst();
    const QPersistentModelIndex index = m_mapThumbnailIndex.value(sPath);
    QStandardItem *pItem = index.isValid() ? m_hModelFilePath->itemFromIndex(index) : nullptr;
    if (pItem == nullptr)
    {
        scheduleNextThumbnail();
        return;
    }
    if (m_mapThumbnailIcons.contains(sPath))
    {
        scheduleNextThumbnail();
        return;
    }

    const FILEINFO info = pItem->data(FileListModel::InfoRole).value<FILEINFO>();
    if (info.nFileLen <= 0 || info.nFileLen > kThumbnailMaxBytes)
    {
        scheduleNextThumbnail();
        return;
    }

    ThumbnailJob job;
    job.sKey       = sPath;
    job.nPos       = info.nContentPositon;
    job.nLen       = info.nFileLen;
    job.nEpoch     = nEpoch;
    job.bPending   = m_fileManager->isPendingEntry(sPath);
    if (job.bPending)
    {
        // 新增未保存条目没有归档偏移，后台按源文件路径读；保存前会再核对大小。
        job.sSourcePath = info.sFilePath;
    }
    else
    {
        job.sArchivePath = m_fileManager->archivePath();
        if (job.sArchivePath.isEmpty())
        {
            scheduleNextThumbnail();
            return;
        }
        quint32 nCrc = 0;
        if (m_fileManager->contentCrcFor(sPath, &nCrc))
        {
            job.bHasCrc = true;
            job.nExpectedCrc = nCrc;
        }
    }

    ++m_nThumbnailGenerated;   // 失败也算一次尝试，避免反复读坏图
    ++m_nThumbnailInFlight;
    {
        QMutexLocker locker(&m_thumbGate->mutex);
        ++m_thumbGate->inFlight;
    }

    QPointer<MainWindow> self(this);
    auto done = [self](const QString &sDonePath, const QImage &img, quint64 nDoneEpoch) {
        if (!self.isNull())
            self->onThumbnailDecoded(sDonePath, img, nDoneEpoch);
    };
    m_thumbPool->start(new ThumbnailDecodeTask(job, m_thumbGate, done));

    // 立即尝试再提交一张（最多两张在途）；后续由完成回调续。
    scheduleNextThumbnail();
}

void MainWindow::onThumbnailDecoded(const QString &sPath, const QImage &img,
                                    quint64 nEpoch)
{
    if (m_nThumbnailInFlight > 0)
        --m_nThumbnailInFlight;

    if (nEpoch == m_nThumbnailEpoch && m_eViewMode == ViewThumbnail
        && !img.isNull() && !m_mapThumbnailIcons.contains(sPath))
    {
        const QPersistentModelIndex index = m_mapThumbnailIndex.value(sPath);
        QStandardItem *pItem = index.isValid() ? m_hModelFilePath->itemFromIndex(index)
                                               : nullptr;
        if (pItem != nullptr)
        {
            // QPixmap 只能在 GUI 线程创建：后台只交回 QImage。
            const QPixmap pm = QPixmap::fromImage(img);
            if (!pm.isNull())
            {
                QIcon icon;
                icon.addPixmap(pm);
                if (m_mapThumbnailIcons.size() >= kThumbnailCacheLimit)
                    m_mapThumbnailIcons.clear();   // 只丢缓存；模型里已设的图标不受影响
                m_mapThumbnailIcons.insert(sPath, icon);
                if (QStandardItem *pNow = m_hModelFilePath->itemFromIndex(index))
                    pNow->setIcon(icon);
            }
        }
    }

    scheduleNextThumbnail();
}

bool MainWindow::waitForThumbnailReads(int nTimeoutMs)
{
    const QSharedPointer<ThumbnailGate> gate = m_thumbGate;
    QElapsedTimer timer;
    timer.start();
    QMutexLocker locker(&gate->mutex);
    while (gate->inFlight > 0)
    {
        const qint64 nLeft = qint64(nTimeoutMs) - timer.elapsed();
        if (nLeft <= 0)
            return false;
        if (!gate->idle.wait(&gate->mutex, QDeadlineTimer(nLeft)))
            return false;
    }
    return true;
}

bool MainWindow::prepareThumbnailsForArchiveWrite(int nTimeoutMs)
{
    // 先让所有在途任务看到新代数（尽早放弃后续解码），再等它们把归档句柄关掉。
    // 不等待事件循环：任务线程直接归还 inFlight，所以保存不会被 GUI 阻塞逻辑卡死。
    cancelThumbnailGeneration();
    return waitForThumbnailReads(nTimeoutMs);
}

// 当前视图（树/平铺/缩略图）的当前项。删除/导出/属性/可用状态统一从这里取，
// 避免三个模式各写一套 currentIndex()。
QAbstractItemView *MainWindow::activeFileListView() const
{
    if (m_eViewMode == ViewThumbnail && m_lvFilePath != nullptr)
        return m_lvFilePath;
    return m_tvFilePath;
}

QModelIndex MainWindow::currentFileListIndex() const
{
    QAbstractItemView *pView = activeFileListView();
    return pView != nullptr ? pView->currentIndex() : QModelIndex();
}

QStandardItem *MainWindow::currentFileListItem() const
{
    if (m_hModelFilePath == nullptr)
        return nullptr;
    return m_hModelFilePath->itemFromIndex(currentFileListIndex().siblingAtColumn(0));
}

bool MainWindow::matchesFilter(const FILEINFO &info) const
{
    if (m_sFilter.isEmpty())
        return true;
    if (info.sFilePath.contains(m_sFilter, Qt::CaseInsensitive)
        || info.sFileName.contains(m_sFilter, Qt::CaseInsensitive))
        return true;
    // v4 标记也参与搜索：用户记住的往往是「合同」这类分类，而不是文件名。
    for (const QString &sTag : info.listTags)
        if (sTag.contains(m_sFilter, Qt::CaseInsensitive))
            return true;
    return false;
}

QString MainWindow::typeLabel(const QString &sFileName)
{
    const QString sSuffix = QFileInfo(sFileName).suffix();
    return sSuffix.isEmpty() ? tr("文件") : sSuffix.toUpper();
}

// 树形模式：把条目里保存的原始路径还原成目录层级
//   根节点 = 所有条目的最长公共父目录（如 E:/study/docs）
//   根下按各条目去掉前缀后的目录段逐级建节点，文件作为叶子
//   只有唯一子目录的链条会被压缩成一行（a -> b -> c 显示为 "a/b/c"）
// 目录节点补齐列：大小=子树合计字节（BytesRole），类型="文件夹"，并标记 IsDirRole。
// 返回本目录下所有文件的字节合计，供父目录继续累加。
static qint64 finalizeTreeRows(QStandardItem *pParent)
{
    if (pParent == nullptr)
        return 0;

    qint64 nTotal = 0;
    for (int r = 0; r < pParent->rowCount(); ++r)
    {
        QStandardItem *pItem = pParent->child(r, 0);
        if (pItem == nullptr)
            continue;

        if (pItem->rowCount() > 0)   // 目录
        {
            pItem->setData(true, FileListModel::IsDirRole);
            const qint64 nSub = finalizeTreeRows(pItem);

            QStandardItem *pSize = pParent->child(r, 1);
            if (pSize == nullptr)
            {
                pSize = new QStandardItem;
                pParent->setChild(r, 1, pSize);
            }
            pSize->setText(FwArc::formatSize(nSub));
            pSize->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
            pSize->setData(nSub, FileListModel::BytesRole);

            QStandardItem *pType = pParent->child(r, 2);
            if (pType == nullptr)
            {
                pType = new QStandardItem;
                pParent->setChild(r, 2, pType);
            }
            pType->setText(QStringLiteral("文件夹"));
            nTotal += nSub;
        }
        else                          // 文件
        {
            // 文件字节数存在第 1 列（大小列）的 BytesRole 上，不是第 0 列
            QStandardItem *pSize = pParent->child(r, 1);
            if (pSize != nullptr)
                nTotal += pSize->data(FileListModel::BytesRole).toLongLong();
        }
    }
    return nTotal;
}

void MainWindow::populateFilePathTree()
{
    // 先记录重建前已展开的目录，重建后恢复，避免增删一个文件导致整棵树被折叠。
    // 换归档后（m_bResetTreeExpansion）不能沿用旧键：展开键是路径文本链，与归档无关，
    // 直接把上一份的目录套到新归档上会"无端展开"。
    const bool bResetExpansion = m_bResetTreeExpansion;
    m_bResetTreeExpansion = false;
    const bool bWasEmpty = (m_hModelFilePath->rowCount() == 0);
    QSet<QString> setExpanded;
    if (!bResetExpansion)
    {
        for (int i = 0; i < m_hModelFilePath->rowCount(); ++i)
            collectExpandedItems(m_hModelFilePath->index(i, 0), QString(), setExpanded);
    }

    m_hModelFilePath->removeRows(0, m_hModelFilePath->rowCount());

    // 按筛选词过滤（树形模式只保留匹配的文件，目录层级随匹配文件重建）
    QVector<FILEINFO> vtFileInfos;
    for (const FILEINFO &info : m_fileManager->getFileInfos())
        if (matchesFilter(info))
            vtFileInfos << info;
    if (vtFileInfos.isEmpty())
        return;
    QStringList prefixSegs = commonDirPrefix(vtFileInfos);
    const QString sRootText = prefixSegs.join('/');

    QIcon iconDir  = style()->standardIcon(QStyle::SP_DirIcon);
    QIcon iconFile = style()->standardIcon(QStyle::SP_FileIcon);

    // 根节点：公共父目录；跨盘符等无公共前缀的情况不建根，直接用盘符/首段作顶层
    QStandardItem *pRoot = nullptr;
    if (!prefixSegs.isEmpty())
    {
        pRoot = new QStandardItem(iconDir, sRootText);
        pRoot->setToolTip(sRootText);
        m_hModelFilePath->setItem(0, 0, pRoot);
    }

    QMap<QString, QStandardItem*> mapDirItems;   // 相对目录路径 -> 目录节点
    for (const FILEINFO &info : vtFileInfos)
    {
        QStringList segs = normalizePath(info.sFilePath).split('/', Qt::SkipEmptyParts);
        if (segs.size() < prefixSegs.size() + 1)
            continue;   // 防御：路径比公共前缀还短（理论上不会出现）

        QStandardItem *pParent = pRoot;
        QString sRelDir;
        for (int i = prefixSegs.size(); i < segs.size() - 1; ++i)
        {
            sRelDir = sRelDir.isEmpty() ? segs.at(i) : sRelDir + "/" + segs.at(i);

            QStandardItem *pDir = mapDirItems.value(sRelDir, nullptr);
            if (pDir == nullptr)
            {
                pDir = new QStandardItem(iconDir, segs.at(i));
                pDir->setToolTip(sRelDir);
                if (pParent != nullptr)
                    pParent->appendRow(pDir);
                else
                    m_hModelFilePath->setItem(m_hModelFilePath->rowCount(), 0, pDir);
                mapDirItems.insert(sRelDir, pDir);
            }
            pParent = pDir;
        }

        QStandardItem *hFile = new QStandardItem(iconFile, info.sFileName);
        QString sTip = normalizePath(info.sFilePath);
        if (!info.listTags.isEmpty())
            sTip += tr("\n标记：%1").arg(tagsColumnText(info));
        hFile->setToolTip(sTip);
        QVariant var;
        var.setValue(info);
        hFile->setData(var, FileListModel::InfoRole);
        QStandardItem *pSize = new QStandardItem(FwArc::formatSize(info.nFileLen));
        pSize->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
        pSize->setData(info.nFileLen, Qt::UserRole);   // 大小列按数值排序用
        QStandardItem *pType = new QStandardItem(typeLabel(info.sFileName));
        QStandardItem *pTags = new QStandardItem(tagsColumnText(info));
        if (!info.listTags.isEmpty())
            pTags->setToolTip(tr("标记：%1").arg(tagsColumnText(info)));

        if (pParent != nullptr)
            pParent->appendRow(QList<QStandardItem *>() << hFile << pSize << pType << pTags);
        else
        {
            const int nRow = m_hModelFilePath->rowCount();
            m_hModelFilePath->setItem(nRow, 0, hFile);
            m_hModelFilePath->setItem(nRow, 1, pSize);
            m_hModelFilePath->setItem(nRow, 2, pType);
            m_hModelFilePath->setItem(nRow, 3, pTags);
        }
    }

    // 单链目录压缩
    for (int i = 0; i < m_hModelFilePath->rowCount(); ++i)
        compactSingleChainDirs(m_hModelFilePath->item(i, 0));

    // 目录节点补上"子树合计大小 / 文件夹类型"，并标记 IsDirRole（树形排序用）
    finalizeTreeRows(m_hModelFilePath->invisibleRootItem());

    // 按表头当前列/方向排序（同级排序、目录优先），再恢复展开状态
    applyCurrentSortToModel();

    // 展开状态：首次加载展开根与顶层；用户主动全折叠后刷新不得强制展开。
    if (bResetExpansion || (setExpanded.isEmpty() && bWasEmpty))
    {
        m_tvFilePath->expandToDepth(0);
        if (pRoot != nullptr)
            m_tvFilePath->setExpanded(m_hModelFilePath->indexFromItem(pRoot), true);
    }
    else
    {
        for (int i = 0; i < m_hModelFilePath->rowCount(); ++i)
            restoreExpandedItems(m_hModelFilePath->index(i, 0), QString(), setExpanded);
        if (pRoot != nullptr)
            m_tvFilePath->setExpanded(m_hModelFilePath->indexFromItem(pRoot), true);
    }
}

// 按「归档是否已加载 + 当前展示模式」刷新工具栏动作可用状态
void MainWindow::updateViewActionState()
{
    const bool bLoaded = m_fileManager->isArchiveLoaded();
    const bool bBusy = m_fileManager->isBusy();
    const bool bCanSwitch = bLoaded && !bBusy;

    m_acViewTree->setEnabled(bCanSwitch && m_eViewMode != ViewTree);
    m_acViewFlat->setEnabled(bCanSwitch && m_eViewMode != ViewFlat);
    m_acViewThumbnail->setEnabled(bCanSwitch && m_eViewMode != ViewThumbnail);
    // 「显示路径/显示名称」在平铺与缩略图模式下都有意义（缩略图标签同样可切换）
    const bool bListLike = bCanSwitch && m_eViewMode != ViewTree;
    m_acShowFilePath->setEnabled(bListLike && !m_bShowPath);
    m_acShowFileName->setEnabled(bListLike && m_bShowPath);
}

// 唯一的状态刷新入口：包含保存按钮与所有菜单动作（P1-5 / P1-6）
void MainWindow::syncActionStates()
{
    const bool bLoaded = m_fileManager->isArchiveLoaded();
    const bool bBusy   = m_fileManager->isBusy();
    QStandardItem *pCur = currentFileListItem();
    const bool bHasLeaf = isFileLeafItem(pCur);

    updateViewActionState();
    // P1-10：只剩一个条目时不允许删除（空归档无法保存），按钮状态与实际行为一致
    const bool bCanDelete = bLoaded && !bBusy && bHasLeaf
                            && m_fileManager->getFileInfos().size() > 1;
    m_acSaveFile->setEnabled(bLoaded && m_fileManager->needsSave() && !bBusy);
    m_acAddFile->setEnabled(bLoaded && !bBusy);
    m_acDelFile->setEnabled(bCanDelete);
    m_acOutputFile->setEnabled(bLoaded && !bBusy && bHasLeaf);

    ui->actionLoadFile->setEnabled(!bBusy);
    ui->actionUnloadFile->setEnabled(bLoaded && !bBusy);
    ui->actionMergeFile->setEnabled(!bBusy);
    ui->actionSplitFile->setEnabled(!bBusy);
    if (m_acVerify != nullptr) m_acVerify->setEnabled(bLoaded && !bBusy);

    // 标题里的“*”跟随未保存状态（所有状态刷新都会走到这里）
    updateWindowTitle();
}

// 长操作期间禁用可重入动作（P3-2）
void MainWindow::setOperationsEnabled(bool bEnabled)
{
    const bool bLoaded = m_fileManager->isArchiveLoaded();
    const bool bBusy   = !bEnabled;
    ui->actionLoadFile->setEnabled(bEnabled);
    ui->actionUnloadFile->setEnabled(bEnabled);
    ui->actionMergeFile->setEnabled(bEnabled);
    ui->actionSplitFile->setEnabled(bEnabled);
    if (bEnabled)
        updateViewActionState();
    else
    {
        m_acViewTree->setEnabled(false);
        m_acViewFlat->setEnabled(false);
        m_acViewThumbnail->setEnabled(false);
        m_acShowFilePath->setEnabled(false);
        m_acShowFileName->setEnabled(false);
    }
    ui->actionUnloadFile->setEnabled(bEnabled && bLoaded);
    QStandardItem *pCur = currentFileListItem();
    const bool bHasLeaf = isFileLeafItem(pCur);
    m_acAddFile->setEnabled(bEnabled && bLoaded);
    m_acDelFile->setEnabled(bEnabled && bLoaded && bHasLeaf);
    m_acOutputFile->setEnabled(bEnabled && bLoaded && bHasLeaf);
    m_acSaveFile->setEnabled(bEnabled && bLoaded && m_fileManager->needsSave() && !bBusy);
    if (m_acVerify != nullptr) m_acVerify->setEnabled(bEnabled && bLoaded);
}

// 统一管理进度对话框的三条连接，避免 disconnect(sender, nullptr, ...) 误伤常驻连接
void MainWindow::beginProgressOperation(QProgressDialog *dlg, bool *pbOk, QString *psResultMsg)
{
    endProgressOperation();
    // 任何新进度操作开始前复位取消标志（否则上一次取消会让这次立刻失败）
    m_fileManager->resetCancel();
    m_pActiveProgress = dlg;
    m_progressTimer.start();
    *pbOk = false;
    psResultMsg->clear();

    // 进度文字左对齐，让「正在合并:」「正在分割:」这类固定前缀稳定贴在左侧。
    //
    // QProgressDialog 的状态 label 默认是 AlignHCenter。对话框宽度在首次 show 后固定，
    // 但状态文本长度随「正在合并: <文件名> (<已写>/<总量>)」变化，文本越宽、居中后
    // 起始位置越靠左，于是「正在合并:」这几个字会随着文件名长短左右漂移。
    // 改为左对齐后前缀始终停在左侧，且与下方进度条左端对齐。
    if (QLabel *pStatusLabel = dlg->findChild<QLabel*>())
    {
        pStatusLabel->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
        // 固定尺寸后 label 必须允许收缩，否则超长路径仍会把窗口撑大
        pStatusLabel->setWordWrap(false);
        pStatusLabel->setMinimumWidth(0);
        pStatusLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    }
    // 固定尺寸：状态文本随文件名/路径变化时弹窗不再忽大忽小（长文本由下方 elidedText 处理）
    dlg->setFixedSize(560, 150);

    // 主题把进度条压成 8px 细条（resources/theme/light.qss），而 Fusion 样式会在
    // 条内居中绘制「8%」——8px 高装不下 10pt 文字，数字被上下裁掉只剩半截。
    // 百分比改由状态文字承载（见下方 progressChanged 中拼接的 " · %1%"），条内不再画字。
    if (QProgressBar *pBar = dlg->findChild<QProgressBar *>())
        pBar->setTextVisible(false);

    m_connProgress = connect(m_fileManager, &FileManager::progressChanged, this,
        [this](qint64 cur, qint64 total, const QString &status) {
            if (m_pActiveProgress == nullptr) return;
            QString sText = status;
            if (total > 0)
            {
                const int nPct = int(100 * cur / total);
                m_pActiveProgress->setValue(nPct);
                // 百分比写进状态文字：细条里画不下（见 beginProgressOperation 里的 setTextVisible）
                sText += tr(" · %1%").arg(nPct);
                // 进度 ETA：进度稳定后按"已用时间 / 完成比例"估算剩余时间
                if (nPct >= 3 && nPct < 100)
                {
                    const qint64 nElapsed = m_progressTimer.elapsed();
                    const qint64 nRemainMs = nElapsed * (100 - nPct) / nPct;
                    if (nRemainMs >= 1000)
                        sText += tr(" · 剩余约 %1 秒").arg(nRemainMs / 1000);
                }
            }
            if (QLabel *pLabel = m_pActiveProgress->findChild<QLabel*>())
            {
                const QFontMetrics fm = pLabel->fontMetrics();
                // 对话框固定 560 宽；用对话框宽度而不是 label 当前宽度，
                // 避免首次 show 前 label 还没拿到最终几何。
                const int nAvail = qMax(200, m_pActiveProgress->width() - 40);
                const QStringList lines = sText.split(QLatin1Char('\n'));
                if (lines.size() >= 2)
                {
                    // 第一行是「正在合并: <路径>」：只省略路径中段，
                    // 保留固定前缀；第二行的大小/剩余时间原样保留。
                    QString sHead = lines.first();
                    const int nSep = sHead.indexOf(QStringLiteral(": "));
                    const QString sPrefix = (nSep >= 0) ? sHead.left(nSep + 2) : QString();
                    QString sPath = (nSep >= 0) ? sHead.mid(nSep + 2) : sHead;
                    const int nPathAvail = qMax(40, nAvail - fm.horizontalAdvance(sPrefix));
                    sPath = fm.elidedText(sPath, Qt::ElideMiddle, nPathAvail);
                    QStringList outLines = lines;
                    outLines[0] = sPrefix + sPath;
                    sText = outLines.join(QLatin1Char('\n'));
                }
                else
                {
                    // 分割/校验等单行状态：整行省略中段，保证末尾大小可见。
                    sText = fm.elidedText(sText, Qt::ElideMiddle, nAvail);
                }
                pLabel->setToolTip(status);      // 鼠标悬停可看完整路径
            }
            m_pActiveProgress->setLabelText(sText);
        });
    m_connFinished = connect(m_fileManager, &FileManager::operationFinished, this,
        [this, pbOk, psResultMsg](bool ok, const QString &msg) {
            if (m_pActiveProgress) { m_pActiveProgress->setValue(100); m_pActiveProgress->close(); }
            *pbOk = ok;
            *psResultMsg = msg;
        });
    m_connCancel = connect(dlg, &QProgressDialog::canceled,
                           m_fileManager, &FileManager::cancelOperation);
}

void MainWindow::endProgressOperation()
{
    disconnect(m_connProgress);
    disconnect(m_connFinished);
    disconnect(m_connCancel);
    m_connProgress = QMetaObject::Connection();
    m_connFinished = QMetaObject::Connection();
    m_connCancel   = QMetaObject::Connection();
    m_pActiveProgress = nullptr;
}

void MainWindow::onActionVerifyArchiveTriggered()
{
    if (!m_fileManager->isArchiveLoaded())
    {
        QMessageBox::information(this, QStringLiteral("校验归档"),
                                 QStringLiteral("请先加载一个归档文件。"));
        return;
    }
    if (m_fileManager->isDirty()
        && QMessageBox::question(this, tr("未保存的改动"),
               tr("当前归档有未保存改动，校验将基于磁盘上的旧归档执行，"
                  "报告中条目数可能与列表不一致。\n是否继续？"),
               QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes)
        return;
    QProgressDialog dlg(QStringLiteral("正在校验归档内容..."), QStringLiteral("取消"), 0, 100, this);
    dlg.setWindowTitle(QStringLiteral("校验归档"));
    dlg.setWindowModality(Qt::WindowModal);
    dlg.setMinimumDuration(0);
    dlg.setAutoClose(false);
    dlg.setAutoReset(false);
    dlg.setValue(0);

    bool bDummy = false;
    QString sDummy;
    beginProgressOperation(&dlg, &bDummy, &sDummy);
    dlg.show();
    QApplication::processEvents();
    setOperationsEnabled(false);

    QString sReport;
    QApplication::setOverrideCursor(Qt::WaitCursor);
    const bool bOk = m_fileManager->verifyArchive(&sReport);
    QApplication::restoreOverrideCursor();

    dlg.close();
    endProgressOperation();
    setOperationsEnabled(true);
    syncActionStates();
    if (bOk) QMessageBox::information(this, QStringLiteral("校验归档"), sReport);
    else     QMessageBox::warning(this, QStringLiteral("校验归档"), sReport);
}

// 离开树形前记住展开状态；切平铺、切缩略图两条路径共用
void MainWindow::captureTreeExpansionState()
{
    m_setExpandedTree.clear();
    for (int i = 0; i < m_hModelFilePath->rowCount(); ++i)
        collectExpandedItems(m_hModelFilePath->index(i, 0), QString(), m_setExpandedTree);
}

// 递归收集当前已展开的节点（键 = 从根到该节点的文本链）
void MainWindow::collectExpandedItems(const QModelIndex &index, const QString &sKeyPrefix, QSet<QString> &setExpanded)
{
    if (!index.isValid())
        return;

    QString sKey = sKeyPrefix.isEmpty() ? index.data().toString()
                                        : sKeyPrefix + "/" + index.data().toString();

    if (m_hModelFilePath->hasChildren(index) && m_tvFilePath->isExpanded(index))
        setExpanded.insert(sKey);

    const int nChild = m_hModelFilePath->rowCount(index);
    for (int i = 0; i < nChild; ++i)
        collectExpandedItems(m_hModelFilePath->index(i, 0, index), sKey, setExpanded);
}

// 递归恢复展开状态
void MainWindow::restoreExpandedItems(const QModelIndex &index, const QString &sKeyPrefix, const QSet<QString> &setExpanded)
{
    if (!index.isValid())
        return;

    QString sKey = sKeyPrefix.isEmpty() ? index.data().toString()
                                        : sKeyPrefix + "/" + index.data().toString();

    if (setExpanded.contains(sKey) && m_hModelFilePath->hasChildren(index))
        m_tvFilePath->setExpanded(index, true);

    const int nChild = m_hModelFilePath->rowCount(index);
    for (int i = 0; i < nChild; ++i)
        restoreExpandedItems(m_hModelFilePath->index(i, 0, index), sKey, setExpanded);
}

void MainWindow::refreshFilePathView()
{
    // populateFilePathView 内部会按当前模式启用/停用排序并重新套用当前排序列
    populateFilePathView(m_bShowPath);
}

// 保存会重写归档并重新计算所有内容偏移；标签页里缓存的是打开时的 FILEINFO 拷贝，
// 必须按路径回填，否则下一条 ≥128MB 的 mmap 会用旧偏移读错位数据。
void MainWindow::refreshTabInfosFromManager()
{
    const QVector<FILEINFO> infos = m_fileManager->getFileInfos();
    for (TabDoc &doc : m_vTabDocs)
    {
        if (doc.info.sFilePath.isEmpty())
            continue;
        bool bFound = false;
        for (const FILEINFO &fi : infos)
        {
            if (fi.sFilePath == doc.info.sFilePath)
            {
                doc.info = fi;
                bFound = true;
                break;
            }
        }
        if (!bFound)
        {
            // pending/跨盘保存后 displayPath 可能变（E:/x -> E/x）；按唯一文件名回填。
            const FILEINFO *pMatch = nullptr;
            int nMatches = 0;
            for (const FILEINFO &fi : infos)
                if (fi.sFileName == doc.info.sFileName) { pMatch = &fi; ++nMatches; }
            if (nMatches == 1)
                doc.info = *pMatch;
            else
                doc.needsRender = false;   // 条目已删除/无法唯一定位，禁止用旧偏移重渲染
        }
    }
}

void MainWindow::onActionViewTreeTriggered()
{
    if (m_eViewMode == ViewTree)
        return;
    m_eViewMode = ViewTree;
    populateFilePathView(m_bShowPath);
    // 从平铺/缩略图切回树形：恢复离开树形前记录的展开状态。
    if (!m_setExpandedTree.isEmpty())
    {
        for (int i = 0; i < m_hModelFilePath->rowCount(); ++i)
            restoreExpandedItems(m_hModelFilePath->index(i, 0), QString(), m_setExpandedTree);
    }
}

void MainWindow::onActionViewFlatTriggered()
{
    if (m_eViewMode == ViewFlat)
        return;
    // 切走会重建模型：先在树形下记住展开状态，切回树形时恢复。
    if (m_eViewMode == ViewTree)
        captureTreeExpansionState();
    m_eViewMode = ViewFlat;
    populateFilePathView(m_bShowPath);
}

void MainWindow::onActionViewThumbnailTriggered()
{
    if (m_eViewMode == ViewThumbnail)
        return;
    if (m_eViewMode == ViewTree)
        captureTreeExpansionState();
    m_eViewMode = ViewThumbnail;
    populateFilePathView(m_bShowPath);
}

void MainWindow::onActionShowFilePathTriggered()
{
    populateFilePathView(true);
}

void MainWindow::onActionShowFileNameTriggered()
{
    populateFilePathView(false);
}

void MainWindow::onActionAddFileTriggered()
{
    if(!m_fileManager->isArchiveLoaded())
    {
        QMessageBox::information(this, tr("添加文件"),
                                 tr("请先通过“加载文件”打开一个归档文件，再添加文件。"));
        return;
    }

    QStringList qLFilePaths = QFileDialog::getOpenFileNames(this, tr("添加文件"),
                                                           "", tr("All Files (*.*)"));
    if(qLFilePaths.isEmpty())
        return;

    QVector<QString> vtAdd;
    foreach(QString s, qLFilePaths)
    {
        QFileInfo fi(s);
        if(fi.isFile())
            vtAdd.append(s);
    }
    if(vtAdd.isEmpty())
        return;

    QApplication::setOverrideCursor(Qt::WaitCursor);
    m_fileManager->addFiles(vtAdd);
    QApplication::restoreOverrideCursor();

    // 新增条目可能覆盖同路径的旧内容（或带来新图片），缩略图缓存不能沿用
    invalidateThumbnailCache();
    refreshFilePathView();
    syncActionStates();
}

void MainWindow::onActionDelFileTriggered()
{
    if(!m_fileManager->isArchiveLoaded())
    {
        QMessageBox::information(this, tr("删除文件"),
                                 tr("请先通过“加载文件”打开一个归档文件。"));
        return;
    }

    // 树形模式下目录节点不可删除，需选中具体文件；列可能不是第 0 列
    QStandardItem *pItem = currentFileListItem();
    if (!isFileLeafItem(pItem))
    {
        QMessageBox::information(this, tr("删除文件"),
                                 tr("请先在列表中选择要删除的文件（目录节点不可删除）。"));
        return;
    }
    FILEINFO fileInfo = pItem->data(FileListModel::InfoRole).value<FILEINFO>();

    // P1-10：空归档语义必须前后一致。删除最后一个条目会让归档无法保存，
    // 所以这里直接拒绝，并指向真正的“清空”入口（卸载文件）。
    if (m_fileManager->getFileInfos().size() <= 1)
    {
        QMessageBox::information(this, tr("删除文件"),
            tr("归档至少需要保留一个条目（空归档无法保存）。\n"
               "如需清空归档，请使用「卸载文件」。"));
        return;
    }

    // P1-10：删除前处理已打开该条目的标签页，避免旧标签继续显示已不存在的快照。
    int nClosedTabs = 0;
    for (int i = ui->tabWidget->count() - 1; i >= 0; --i)
    {
        const int nDoc = docIndexForTab(i);
        if (nDoc < 0 || nDoc >= m_vTabDocs.size())
            continue;
        if (!m_vTabDocs.at(nDoc).hasFile
            || m_vTabDocs.at(nDoc).info.sFilePath != fileInfo.sFilePath)
            continue;
        // 先安全停止（停不下来会转入延期清理，不会 delete/悬空）
        ensureInstanceStopped(m_vTabDocs.at(nDoc).interface);
        destroyTabAt(i);
        ++nClosedTabs;
    }
    if (ui->tabWidget->count() == 0)
        newEmptyTab();

    QApplication::setOverrideCursor(Qt::WaitCursor);
    bool bOk = m_fileManager->deleteFile(fileInfo.sFilePath);
    QApplication::restoreOverrideCursor();

    if(!bOk)
    {
        QMessageBox::critical(this, tr("删除文件"),
                              tr("删除失败，归档文件可能已损坏。"));
        return;
    }
    if (nClosedTabs > 0)
    {
        statusBar()->showMessage(
            tr("已删除条目，并关闭 %1 个正在显示它的标签页").arg(nClosedTabs), 6000);
    }

    invalidateThumbnailCache();
    refreshFilePathView();
    syncActionStates();
}

void MainWindow::onActionOutputFileTriggered()
{
    // 树形模式下目录节点不可导出，需选中具体文件；列可能不是第 0 列
    QStandardItem *pItem = currentFileListItem();
    if (!isFileLeafItem(pItem))
    {
        QMessageBox::information(this, tr("导出文件"),
                                 tr("请先在列表中选择要导出的文件（目录节点不可导出）。"));
        return;
    }
    FILEINFO fileInfo = pItem->data(FileListModel::InfoRole).value<FILEINFO>();

    DlgOutputFile dlgOutputFile(this);
    if(dlgOutputFile.exec() == QDialog::Accepted)
    {
        const QString sOutputDir = dlgOutputFile.getOutputDir();

        QProgressDialog dlgProgress(QStringLiteral("正在导出文件..."), QStringLiteral("取消"), 0, 100, this);
        dlgProgress.setWindowTitle(QStringLiteral("导出文件"));
        dlgProgress.setWindowModality(Qt::WindowModal);
        dlgProgress.setMinimumDuration(0);
        dlgProgress.setAutoClose(false);
        dlgProgress.setAutoReset(false);
        dlgProgress.setValue(0);
        bool bDummy = false;
        QString sDummy;
        beginProgressOperation(&dlgProgress, &bDummy, &sDummy);
        dlgProgress.show();
        QApplication::processEvents();
        setOperationsEnabled(false);

        m_fileManager->outputFile(fileInfo.sFilePath, sOutputDir);
        const QString sErr = m_fileManager->lastError();

        dlgProgress.close();
        endProgressOperation();
        setOperationsEnabled(true);
        syncActionStates();

        if (!sErr.isEmpty())
            QMessageBox::warning(this, QStringLiteral("导出文件"), sErr);
        else
        {
            const QString sActual = m_fileManager->lastOutputPath();
            QMessageBox::information(this, QStringLiteral("导出文件"),
                                     QStringLiteral("导出完成：%1").arg(sActual.isEmpty() ? fileInfo.sFileName : sActual));
        }
    }
}

void MainWindow::onActionSaveFileTriggered()
{
    if(!m_fileManager->isArchiveLoaded())
    {
        QMessageBox::information(this, tr("保存文件"),
                                 tr("没有已加载的归档文件，无法保存。"));
        return;
    }

    // 后台 CRC 仍在读归档时不能改名重写（Windows 共享冲突）：先等它结束；
    // 超时给出明确提示，避免保存只报"被占用"却查不出原因。
    if (!waitForMappedCrc(15000))
    {
        QMessageBox::warning(this, tr("保存文件"),
            tr("后台仍在校验归档内容，暂时无法保存。\n请稍等几秒后重试。"));
        return;
    }

    // 有视频/音频正在直接读取归档（内存映射）时，保存需要先停止播放并释放映射：
    // Windows 下正在映射的文件无法被改名/替换，不释放会直接导致保存失败。
    if (!m_mapMappedInputs.isEmpty())
    {
        const auto btn = QMessageBox::question(this, tr("保存文件"),
            tr("有视频/音频正在直接读取归档内容。保存需要先停止播放并释放该文件，\n"
               "播放会中断（可重新双击打开）。是否继续保存？"),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (btn != QMessageBox::Yes)
            return;
        stopCurrentPlaybackAndInvalidate();
        releaseAllMappedInputs();
    }

    // P0-3：停播未被确认时，映射仍可能被后台线程访问，不能替换归档。
    // 先非阻塞复查一次，避免后台线程已经退出、界面却一直拒绝保存。
    pollStopUnsafeInstances();
    if (!m_setStopUnsafe.isEmpty())
    {
        QMessageBox::warning(this, tr("保存文件"),
            tr("有插件的后台播放未能确认停止，其内存映射仍在使用。\n"
               "此时替换归档可能失败或产生不一致，已取消本次操作。\n"
               "程序会每秒自动复查；也可关闭正在播放的标签页后重试。"));
        return;
    }

    // 缩略图后台任务也持有归档的只读句柄；Windows 下未关闭会让 rename 失败。
    // 取消尚未开始的任务并等待在途任务结束（最多 5 秒），失败则明确中止本次保存。
    if (!prepareThumbnailsForArchiveWrite(5000))
    {
        QMessageBox::warning(this, tr("保存文件"),
            tr("缩略图仍在读取归档内容，暂时无法替换文件。\n请稍等几秒后重试。"));
        refreshFilePathView();   // 恢复待生成的缩略图
        return;
    }

    // —— 进度条：保存（重写归档）可能耗时，显示进度并支持取消 ——
    QProgressDialog dlgProgress(tr("正在保存文件..."), tr("取消"), 0, 100, this);
    dlgProgress.setWindowTitle(tr("保存文件"));
    dlgProgress.setWindowModality(Qt::WindowModal);
    dlgProgress.setMinimumDuration(0);
    dlgProgress.setAutoClose(false);
    dlgProgress.setAutoReset(false);
    dlgProgress.setValue(0);

    bool bDummy = false;
    QString sDummy;
    beginProgressOperation(&dlgProgress, &bDummy, &sDummy);

    dlgProgress.show();
    QApplication::processEvents();
    setOperationsEnabled(false);

    const bool bOk = m_fileManager->save();
    const QString sReason = m_fileManager->lastError();

    dlgProgress.close();
    endProgressOperation();
    setOperationsEnabled(true);
    syncActionStates();

    if(!bOk)
    {
        // 进度对话框关闭时也可能发出 canceled()，不能只用 isCancelRequested 判断；
        // 优先采用 FileManager 给出的具体原因，只有确实是"操作已取消"才提示已取消。
        const bool bCancelled = m_fileManager->isCancelRequested()
                                && (sReason.isEmpty() || sReason.contains(tr("取消")));
        QString sMsg = bCancelled
                ? tr("保存已取消。改动仍保留在内存中，可重试。")
                : tr("保存失败。改动仍保留在内存中，可重试。\n\n原因：%1")
                    .arg(sReason.isEmpty() ? tr("未知错误") : sReason);
        refreshFilePathView();   // 保存前取消了缩略图任务，失败后恢复
        QMessageBox::critical(this, tr("保存文件"), sMsg);
        return;
    }

    afterArchiveSaved();
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    // 分割/合并/保存可能持续数分钟；期间 FileManager 会通过 processEvents
    // 回到事件循环。若此时允许关闭主窗口，后续 progressChanged 会回调已销毁
    // 的 MainWindow（进度框/状态栏），造成崩溃。必须等操作结束或先在进度框取消。
    if (m_fileManager->isBusy())
    {
        QMessageBox::information(this, tr("操作进行中"),
            tr("正在合并/分割/保存归档，请等待完成，或先在进度对话框中点击“取消”。"));
        event->ignore();
        return;
    }
    if (m_bScanningFiles)
    {
        event->ignore();
        statusBar()->showMessage(tr("正在扫描待合并文件，请先取消进度框"), 3000);
        return;
    }
    if (!confirmDiscardOrSaveIfDirty())
    {
        event->ignore();
        return;
    }
    saveUiState();
    // 先释放插件实例（停止音视频播放、清理临时副本），再真正退出
    closeAllTabs();
    event->accept();
}

void MainWindow::restoreUiState()
{
    QSettings st(QSettings::IniFormat, QSettings::UserScope,
                 QStringLiteral("FileWrapper"), QStringLiteral("FileWrapper"));
    st.beginGroup(QStringLiteral("MainWindow"));
    const QByteArray geo = st.value(QStringLiteral("geometry")).toByteArray();
    if (!geo.isEmpty()) restoreGeometry(geo);
    const QByteArray state = st.value(QStringLiteral("state")).toByteArray();
    if (!state.isEmpty()) restoreState(state);
    // 新版写 viewMode；旧配置只有 treeMode（true/false），按它回退成树形/平铺。
    const QString sMode = st.value(QStringLiteral("viewMode")).toString();
    if (sMode == QLatin1String("flat"))
        m_eViewMode = ViewFlat;
    else if (sMode == QLatin1String("thumbnail"))
        m_eViewMode = ViewThumbnail;
    else if (sMode == QLatin1String("tree"))
        m_eViewMode = ViewTree;
    else
        m_eViewMode = st.value(QStringLiteral("treeMode"), true).toBool() ? ViewTree : ViewFlat;
    m_bShowPath = st.value(QStringLiteral("showPath"), false).toBool();
    st.endGroup();
    if (m_swFilePathViews != nullptr)
    {
        m_swFilePathViews->setCurrentWidget(m_eViewMode == ViewThumbnail
                                                ? static_cast<QWidget *>(m_lvFilePath)
                                                : static_cast<QWidget *>(m_tvFilePath));
    }
    updateViewActionState();
}

// 旧版本把「后缀 -> 插件」映射持久化在 QSettings 的 [PluginMap] 组里。
// 自动路由（内容优先 + 插件声明）已经取代它，运行时不再读取；但残留键会让
// 用户/排查者误以为它还在生效，所以启动时删一次（之后每次启动都是 no-op）。
// 只删这一个组：同文件里的 [MainWindow] 与 [PluginEnabled] 仍在使用。
void MainWindow::purgeLegacyPluginMapSettings()
{
    QSettings st(QSettings::IniFormat, QSettings::UserScope,
                 QStringLiteral("FileWrapper"), QStringLiteral("FileWrapper"));
    if (!st.childGroups().contains(QStringLiteral("PluginMap"), Qt::CaseSensitive))
        return;
    st.remove(QStringLiteral("PluginMap"));
    st.sync();
    qInfo() << "[Settings] 已清理废弃的「插件映射」配置（[PluginMap]）";
}

void MainWindow::saveUiState()
{
    QSettings st(QSettings::IniFormat, QSettings::UserScope,
                 QStringLiteral("FileWrapper"), QStringLiteral("FileWrapper"));
    st.beginGroup(QStringLiteral("MainWindow"));
    st.setValue(QStringLiteral("geometry"), saveGeometry());
    st.setValue(QStringLiteral("state"), saveState());
    const QString sMode = (m_eViewMode == ViewThumbnail) ? QStringLiteral("thumbnail")
                        : (m_eViewMode == ViewFlat)      ? QStringLiteral("flat")
                                                         : QStringLiteral("tree");
    st.setValue(QStringLiteral("viewMode"), sMode);
    // 兼容旧版读取逻辑（只认 treeMode）：平铺/缩略图都按"非树形"保存
    st.setValue(QStringLiteral("treeMode"), m_eViewMode == ViewTree);
    st.setValue(QStringLiteral("showPath"), m_bShowPath);
    st.endGroup();
}

void MainWindow::on_actionAbout_triggered()
{
    DlgAbout dlgAbout;
    dlgAbout.setExtraInfo(tr(
        "<hr><p><b>版本</b>：1.0（%1 构建）<br>"
        "<b>已加载插件</b>：%2 个<br>"
        "<b>Qt</b>：%3</p>")
        .arg(QStringLiteral(__DATE__))
        .arg(PluginManager::getInstance()->getAllInterfaces().size())
        .arg(QString::fromLatin1(qVersion())));
    dlgAbout.exec();
}

void MainWindow::on_actionUnloadFile_triggered()
{
    if (m_fileManager->isBusy())
    {
        QMessageBox::information(this, tr("操作进行中"),
            tr("正在保存/合并归档，请等待完成后再执行其它文件操作。"));
        return;
    }

    if (!confirmDiscardOrSaveIfDirty())
        return;

    m_fileManager->unLoadMergedFile();
    invalidateThumbnailCache();
    m_hModelFilePath->removeRows(0, m_hModelFilePath->rowCount());
    // 卸载后模型已清空：树展开状态也一并作废，避免下次加载同名目录被无端展开
    m_setExpandedTree.clear();
    m_bResetTreeExpansion = true;
    m_tvFilePath->header()->setVisible(false);
    updateWindowTitle();
    if (m_leFilter && !m_leFilter->text().isEmpty())
    {
        QSignalBlocker blocker(m_leFilter);
        m_leFilter->clear();
        m_sFilter.clear();
    }
    syncActionStates();

    // 卸载归档后关闭所有已打开的文件标签页
    closeAllTabs();
}
