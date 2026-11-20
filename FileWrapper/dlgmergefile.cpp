#include "dlgmergefile.h"
#include "ui_dlgmergefile.h"
#include <QFileDialog>
#include <QMessageBox>
#include <QFileInfo>
#include <QDirIterator>
#include <QShortcut>
#include <QTreeView>
#include <QLabel>
#include <QItemSelectionModel>
#include <QScreen>
#include <QDir>
#include <QTimer>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QMimeData>
#include <QResizeEvent>
#include <QUrl>
#include <QThreadPool>
#include <QRunnable>
#include <QPointer>
#include <QCoreApplication>
#include "archiveformat.h"

#include <algorithm>
#include <functional>
#include <utility>
#include "icons.h"

namespace {

// 一轮摘要统计：目录遍历放在工作线程，算完再投递回 GUI 线程改 label。
// 以前 updateSummary() 在 UI 线程里同步递归，添加大文件夹时列表要等扫描结束
// 才会重绘，表现就是「添加文件夹后待合并列表更新很慢」。
class MergeSummaryTask : public QRunnable
{
public:
    using Done = std::function<void(const DlgMergeFile::MergeSummary &, quint64)>;

    MergeSummaryTask(QStringList paths, QSharedPointer<QAtomicInt> abort, quint64 nGeneration,
                     Done done)
        : m_paths(std::move(paths)), m_abort(std::move(abort)),
          m_nGeneration(nGeneration), m_done(std::move(done))
    {
        setAutoDelete(true);
    }

    void run() override
    {
        DlgMergeFile::MergeSummary summary;
        try
        {
            summary = DlgMergeFile::computeMergeSummary(m_paths, m_abort.data());
        }
        catch (...)
        {
            return;   // 后台线程不能让异常逃逸，否则 std::terminate
        }

        // 已作废（又加了路径 / 对话框已关闭）就别再投递，结果也没人用
        if (!m_abort || m_abort->loadAcquire() != 0 || !m_done)
            return;

        QCoreApplication *pApp = QCoreApplication::instance();
        if (pApp == nullptr)
            return;   // 应用正在退出

        const Done done = m_done;
        const quint64 nGeneration = m_nGeneration;
        QMetaObject::invokeMethod(pApp, [done, summary, nGeneration]() {
            done(summary, nGeneration);
        }, Qt::QueuedConnection);
    }

private:
    QStringList m_paths;
    QSharedPointer<QAtomicInt> m_abort;
    quint64 m_nGeneration = 0;
    Done m_done;
};

} // namespace

DlgMergeFile::DlgMergeFile(QWidget *parent) :
    QDialog(parent),
    ui(new Ui::DlgMergeFile)
{
    ui->setupUi(this);

    // 路径提示条会随窗口宽度换行；高度不能依赖 QLabel 的 sizeHint（它是按旧宽度
    // 算的），否则把窗口缩窄后最后一行会被裁掉。resizeEvent 里按当前宽度重算。
    ui->labelResolvedPath->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Minimum);

    ui->btnAddFile->setIcon(FwIcons::get(QStringLiteral("add")));
    ui->btnDelete->setIcon(FwIcons::get(QStringLiteral("delete")));
    ui->btnAddDir->setIcon(FwIcons::get(QStringLiteral("add_dir")));
    ui->btnMergedFilePath->setIcon(FwIcons::get(QStringLiteral("browse")));
    // 确定/取消不加图标：确定是默认按钮（部分对话框为实心蓝底），固定深色图标
    // 会与底色混在一起；文字按钮在三个对话框里更一致。

    // 只有「确定」是默认按钮。
    //
    // QDialog 里的 QPushButton 默认 autoDefault=true，对话框打开时会把焦点链上的
    // 第一个按钮当作默认按钮，套用主题里 QPushButton:default 的蓝色背景——
    // 表现为「添加文件夹」一打开就是蓝底，像是被选中/主操作，和其它操作按钮不一致。
    // 这里显式让操作类按钮不参与默认按钮竞争，Enter 始终对应「确定」。
    for (QPushButton *pBtn : {ui->btnAddDir, ui->btnAddFile, ui->btnDelete,
                              ui->btnMergedFilePath, ui->btnCancel})
        pBtn->setAutoDefault(false);
    ui->btnOk->setDefault(true);

    this->setWindowFlags(windowFlags() & ~Qt::WindowContextHelpButtonHint);

    // 自适应尺寸：固定 1000×600 在小屏/高缩放下可能超出可用区域（4K@150% 实测 1500×900）
    const QRect avail = screen() ? screen()->availableGeometry() : QRect(0, 0, 1280, 800);
    // 小屏/高缩放下不能再用 qMax(720/420) 兜底，否则可用区 <720 时必然越界。
    int w = qMin(1000, qMax(320, avail.width() - 80));
    int h = qMin(600,  qMax(260, avail.height() - 80));
    if (avail.width()  > 40 && w > avail.width()  - 20) w = qMax(200, avail.width()  - 20);
    if (avail.height() > 40 && h > avail.height() - 20) h = qMax(180, avail.height() - 20);
    resize(w, h);

    m_hModelFilePath = new QStandardItemModel(this);
    m_hModelFilePath->setColumnCount(1);
    m_hModelFilePath->setHeaderData(0, Qt::Horizontal, tr("待合并的文件路径"));
    ui->treeView->setModel(m_hModelFilePath);
    // 多选 + 整行选中；按 Delete 键等同于点「删除」
    ui->treeView->setSelectionMode(QAbstractItemView::ExtendedSelection);
    ui->treeView->setSelectionBehavior(QAbstractItemView::SelectRows);

    auto *hDeleteShortcut = new QShortcut(QKeySequence::Delete, ui->treeView);
    connect(hDeleteShortcut, &QShortcut::activated, this, &DlgMergeFile::on_btnDelete_clicked);

    // 拖放：文件/文件夹直接拖进列表即可加入（比点两次按钮快）
    ui->treeView->setAcceptDrops(true);
    ui->treeView->viewport()->setAcceptDrops(true);
    ui->treeView->viewport()->installEventFilter(this);

    // 空列表时直接把提示画在树区域中央：以前只有 tooltip，用户必须把鼠标停在
    // 空白区域才看得到「可直接把文件或文件夹拖到这里」，实际上很难发现。
    m_lblDropHint = new QLabel(tr("可直接把文件或文件夹拖到这里"), ui->treeView->viewport());
    m_lblDropHint->setObjectName(QStringLiteral("lblDropHint"));
    m_lblDropHint->setAlignment(Qt::AlignCenter);
    m_lblDropHint->setWordWrap(true);
    m_lblDropHint->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    m_lblDropHint->setStyleSheet(
        QStringLiteral("QLabel#lblDropHint { color: #8A94A6; background: transparent; }"));
    updateDropHint();

    connect(m_hModelFilePath, &QAbstractItemModel::rowsInserted, this,
            [this](const QModelIndex &, int, int) {
                updateButtonStates();
                updateDropHint();
                updateResolvedPath();   // 输入列表变了，"输出是否落在输入目录内"也要重判
            });
    connect(m_hModelFilePath, &QAbstractItemModel::rowsRemoved, this,
            [this](const QModelIndex &, int, int) {
                updateButtonStates();
                updateDropHint();
                updateResolvedPath();
            });
    connect(ui->treeView->selectionModel(), &QItemSelectionModel::selectionChanged, this,
            [this](const QItemSelection &, const QItemSelection &) { updateButtonStates(); });
    connect(ui->leMergedFilePath, &QLineEdit::textChanged, this,
            [this](const QString &) { updateButtonStates(); updateResolvedPath(); });

    // 摘要统计的异步管线：大目录扫描放到工作线程，避免添加文件夹时卡住界面。
    m_summaryPool = new QThreadPool(this);
    m_summaryPool->setMaxThreadCount(1);      // 统计串行，多轮扫描不互相抢 I/O

    m_summaryDebounce = new QTimer(this);
    m_summaryDebounce->setSingleShot(true);
    m_summaryDebounce->setInterval(120);      // 连续拖入多项时只统计最后一次
    connect(m_summaryDebounce, &QTimer::timeout, this, &DlgMergeFile::startSummaryScan);

    // 小目录几十毫秒就统计完，不要闪一下「正在统计…」；超过 150ms 才给这个提示。
    m_summarySlowHint = new QTimer(this);
    m_summarySlowHint->setSingleShot(true);
    m_summarySlowHint->setInterval(150);
    connect(m_summarySlowHint, &QTimer::timeout, this, [this]() {
        if (m_bSummaryRunning)
            ui->labelSummary->setText(tr("正在统计…"));
    });

    updateSummary();
    updateButtonStates();
    updateResolvedPath();

    // 解析后的路径提示用内联样式：灰=正常，红=不可用（颜色由 updateResolvedPath 切换）
    ui->labelResolvedPath->setStyleSheet(
        QStringLiteral("QLabel#labelResolvedPath { color: #6B7280; }"));
}

DlgMergeFile::~DlgMergeFile()
{
    // 先作废再等：扫描线程每 1024 个文件检查一次 aborted，很快就能退出，
    // 关对话框不会被「正在统计一个大目录」拖住。
    if (m_summaryAbort)
        m_summaryAbort->storeRelease(1);
    if (m_summaryPool != nullptr)
        m_summaryPool->waitForDone(300);

    delete ui;
}

QStringList DlgMergeFile::getFilePaths()
{
    QStringList qLFilePaths;
    int nRowCount = m_hModelFilePath->rowCount();
    for(int i = 0; i < nRowCount; i++)
    {
        qLFilePaths << m_hModelFilePath->item(i, 0)->text();
    }
    return qLFilePaths;
}

QString DlgMergeFile::getMergedFilePath()
{
    return ui->leMergedFilePath->text();
}

// 没有扩展名就补 .fwda（归档的专属后缀，与文件头魔数 FWDA 同名）。
// 用户可能只打了 "a"，落盘成无后缀文件后：
//   - 程序自己的「加载文件」对话框默认只看 *.fwda，找不到它；
//   - 任何按后缀关联的文件管理器都打不开它。
// 已有扩展名的（包括 "v1.2" 这种用点分隔的版本号）一律不动，避免改坏用户意图。
QString DlgMergeFile::ensureArchiveSuffix(const QString &sPath)
{
    const QFileInfo fi(sPath);
    if (!fi.suffix().isEmpty())
        return sPath;

    QString sBase = sPath;
    while (!sBase.isEmpty()
           && (sBase.endsWith(QLatin1Char('.')) || sBase.endsWith(QLatin1Char(' '))))
        sBase.chop(1);            // "a." → "a"，不要写出 "a..fwda"
    return sBase + QLatin1String(FwArc::FILE_SUFFIX);
}

void DlgMergeFile::addPathItem(const QString &sPath, bool bUpdateSummary)
{
    if (sPath.isEmpty() || !duplicatCheck(sPath))
        return;
    m_hModelFilePath->setItem(m_hModelFilePath->rowCount(), 0,
                              new QStandardItem(sPath));
    if (bUpdateSummary)
        updateSummary();
}

// 空列表时在树区域中央显示拖放提示（有内容后隐藏，避免遮挡）
void DlgMergeFile::updateDropHint()
{
    if (m_lblDropHint == nullptr || ui->treeView == nullptr)
        return;

    QWidget *pViewport = ui->treeView->viewport();
    const bool bEmpty = (m_hModelFilePath->rowCount() <= 0);
    m_lblDropHint->setVisible(bEmpty);
    if (!bEmpty)
        return;

    const int nW = qMax(120, qMin(420, pViewport->width() - 24));
    const int nH = qMax(18, m_lblDropHint->heightForWidth(nW));
    m_lblDropHint->setGeometry((pViewport->width() - nW) / 2,
                               (pViewport->height() - nH) / 2,
                               nW, nH);
    m_lblDropHint->raise();
}

// 摘要：输入项数、展开后的文件数/总大小、其中多少个是已合并归档
void DlgMergeFile::updateButtonStates()
{
    const bool bHasRows = (m_hModelFilePath->rowCount() > 0);
    const bool bHasSel = (ui->treeView->selectionModel() != nullptr)
                         && ui->treeView->selectionModel()->hasSelection();
    const bool bHasOutput = !ui->leMergedFilePath->text().trimmed().isEmpty();
    ui->btnDelete->setEnabled(bHasSel);
    ui->btnOk->setEnabled(bHasRows && bHasOutput);
}

// 输出路径的实时反馈：把相对路径解析成绝对路径，并提前暴露「目录 / 与输入冲突」
// 这类硬性错误。以前只有点「确定」或合并结束才暴露，用户填了 "a" 之后
// 文件落在进程当前目录（从 IDE 启动时就是 build/.../debug），提示里又只有文件数，
// 于是"合并完了但找不到文件"。
void DlgMergeFile::resizeEvent(QResizeEvent *pEvent)
{
    QDialog::resizeEvent(pEvent);
    // resizeEvent 到达时子控件布局可能还没按新宽度算完，必须等这一轮布局结束
    // 再读 label->width()；否则会把旧宽度对应的高度锁进 minimumHeight。
    if (!m_bPathHeightPending)
    {
        m_bPathHeightPending = true;
        QTimer::singleShot(0, this, [this]() {
            m_bPathHeightPending = false;
            adjustResolvedPathHeight();
        });
    }
}

void DlgMergeFile::adjustResolvedPathHeight()
{
    if (m_bAdjustingPathHeight || ui->labelResolvedPath == nullptr)
        return;
    const int nWidth = ui->labelResolvedPath->width();
    if (nWidth <= 0)
        return;

    // QLabel::heightForWidth() 会被当前 minimumHeight 反向影响（设成 182 后它
    // 仍返回 182，窗口变宽也降不下来）。所以先清零最小高度，再按当前宽度算。
    m_bAdjustingPathHeight = true;
    ui->labelResolvedPath->setMinimumHeight(0);
    ui->labelResolvedPath->setMaximumHeight(QWIDGETSIZE_MAX);
    const int nHeight = ui->labelResolvedPath->heightForWidth(nWidth);
    if (nHeight >= 0)
    {
        // 同时锁定最大高度：只设 minimum 时布局会把多余高度继续塞给 label，
        // 窗口拉宽后文字虽变回一行、label 却仍占着窄窗口时的高度。
        ui->labelResolvedPath->setMinimumHeight(nHeight);
        ui->labelResolvedPath->setMaximumHeight(nHeight);
    }
    m_bAdjustingPathHeight = false;
}

void DlgMergeFile::updateResolvedPath()
{
    if (ui->labelResolvedPath == nullptr)
        return;

    const QString sText = ui->leMergedFilePath->text().trimmed();
    if (sText.isEmpty())
    {
        ui->labelResolvedPath->clear();
        ui->labelResolvedPath->setToolTip(QString());
        adjustResolvedPathHeight();
        return;
    }

    const QString sProblem = outputPathProblem(sText);
    QString sHint;
    bool bUnusable = false;

    if (!sProblem.isEmpty())
    {
        // 只有第一行进提示条，多行完整原因留给点「确定」时的弹窗
        sHint = sProblem.section(QLatin1Char('\n'), 0, 0);
        bUnusable = true;
    }
    else
    {
        const QString sAbs = QDir::cleanPath(QFileInfo(sText).absoluteFilePath());
        sHint = tr("将写入：%1").arg(sAbs);

        QStringList notes;
        if (QDir::isRelativePath(sText))
            notes << tr("相对路径，按当前工作目录解析");
        if (QFileInfo(sText).suffix().isEmpty())
            notes << tr("没有扩展名，确定时会询问是否追加 .fwda");
        else if (QFileInfo::exists(sText))
            notes << tr("文件已存在，合并后会替换它");
        if (!notes.isEmpty())
            sHint += QStringLiteral("\n") + notes.join(QStringLiteral(" · "));
    }

    ui->labelResolvedPath->setStyleSheet(
        bUnusable ? QStringLiteral("QLabel#labelResolvedPath { color: #C0392B; }")
                  : QStringLiteral("QLabel#labelResolvedPath { color: #6B7280; }"));
    ui->labelResolvedPath->setText(sHint);
    adjustResolvedPathHeight();
    // 路径很长时提示条会折行；tooltip 给出不折行的完整内容
    ui->labelResolvedPath->setToolTip(sProblem.isEmpty()
        ? QDir::cleanPath(QFileInfo(sText).absoluteFilePath())
        : sProblem);
}

// 输出路径的硬性错误；空串表示可用。
QString DlgMergeFile::outputPathProblem(const QString &sPath) const
{
    const QString sOut = sPath.trimmed();
    if (sOut.isEmpty())
        return tr("合并后的文件路径为空");

    // 以分隔符结尾说明用户想输出到目录（或只是多打了一个反斜杠）。
    // 这种路径交给底层会拼成 "C:/out/.fwda" 之类的怪文件，直接拒绝。
    if (sOut.endsWith(QLatin1Char('/')) || sOut.endsWith(QLatin1Char('\\')))
        return tr("输出路径不能以目录分隔符结尾：\n%1\n\n"
                  "请写完整的文件名（例如 ...\\合并结果.fwda）。").arg(sOut);

    // 输出必须是文件路径：QFileInfo::exists() 对目录也为真，
    // 若把目录交给底层，QFile::rename 会把整个目录移走后再写归档。
    if (QFileInfo(sOut).isDir())
        return tr("输出路径是一个已存在的目录：\n%1\n\n"
                  "请选择一个文件路径（例如 ...\\合并结果.fwda），不能直接输出到目录。").arg(sOut);

    // P0-1：输出文件不能是任一输入文件。
    // 目录输入不再递归枚举全部文件（大目录会在点确定后卡很久才出现进度框）：
    // 只要输出路径位于输入目录内，就必然可能覆盖输入，直接拒绝。
    const QString outKey = QDir::cleanPath(QFileInfo(sOut).absoluteFilePath()).toLower();
    for (int i = 0; i < m_hModelFilePath->rowCount(); ++i)
    {
        const QStandardItem *pItem = m_hModelFilePath->item(i, 0);
        if (pItem == nullptr)
            continue;
        const QString sItem = pItem->text();
        const QFileInfo info(sItem);
        const QString sItemAbs = QDir::cleanPath(info.absoluteFilePath()).toLower();
        if (info.isDir())
        {
            if (outKey == sItemAbs
                || outKey.startsWith(sItemAbs + QLatin1Char('/')))
            {
                return tr("输出文件不能位于待合并的输入目录内：%1\n\n"
                          "合并会先清空输出文件再读取输入，导致源数据丢失。请选择其它输出路径。")
                           .arg(sItem);
            }
        }
        else if (sItemAbs == outKey)
        {
            return tr("输出文件不能是待合并文件之一：%1\n\n"
                      "合并会先清空输出文件再读取输入，导致数据丢失。请选择其它输出路径。")
                       .arg(sItem);
        }
    }
    return QString();
}

// 输入列表变化后重新统计摘要。
// 这里只做三件事：作废旧的一轮、起防抖定时器、让结果回来时再填 label；
// 真正的目录遍历在 MergeSummaryTask 里（工作线程），不再阻塞「添加文件夹」。
void DlgMergeFile::updateSummary()
{
    if (m_summaryAbort)
        m_summaryAbort->storeRelease(1);        // 上一轮结果不再采用
    m_summaryAbort = QSharedPointer<QAtomicInt>::create(0);
    ++m_summaryGeneration;
    m_bSummaryRunning = false;
    if (m_summarySlowHint)
        m_summarySlowHint->stop();

    if (m_hModelFilePath->rowCount() <= 0)
    {
        ui->labelSummary->clear();
        if (m_summaryDebounce)
            m_summaryDebounce->stop();
        return;
    }
    if (m_summaryDebounce)
        m_summaryDebounce->start();
}

void DlgMergeFile::startSummaryScan()
{
    QStringList paths;
    const int nRows = m_hModelFilePath->rowCount();
    paths.reserve(nRows);
    for (int i = 0; i < nRows; ++i)
    {
        const QStandardItem *pItem = m_hModelFilePath->item(i, 0);
        if (pItem != nullptr)
            paths << pItem->text();
    }
    if (paths.isEmpty())
    {
        ui->labelSummary->clear();
        return;
    }

    const quint64 nGeneration = m_summaryGeneration;
    m_bSummaryRunning = true;
    m_summarySlowHint->start();
    m_summaryPool->start(new MergeSummaryTask(
        paths, m_summaryAbort, nGeneration,
        [guard = QPointer<DlgMergeFile>(this)](const DlgMergeFile::MergeSummary &summary,
                                              quint64 nGen) {
            if (guard != nullptr)
                guard->applySummary(summary, nGen);
        }));
}

void DlgMergeFile::applySummary(const MergeSummary &summary, quint64 nGeneration)
{
    if (nGeneration != m_summaryGeneration)
        return;                                  // 期间输入又变了：这一轮作废
    m_bSummaryRunning = false;
    m_summarySlowHint->stop();

    // 摘要单独一行（标题下面），不要拼进「待合并的文件路径」里：
    // 统计文字会随添加的目录变化，拼在标题里会不断改标题宽度、把右侧文件树挤得左右跳。
    QString sSummary;
    if (summary.nInputs > 0)
    {
        sSummary = tr("%1 个输入，约 %2 个文件，%3%4")
                       .arg(summary.nInputs)
                       .arg(summary.bCapped
                                ? QStringLiteral(">") + QString::number(kSummaryFileCap)
                                : QString::number(summary.nFiles))
                       .arg(FwArc::formatSize(summary.nBytes))
                       .arg(summary.nArchives > 0
                                ? tr("（其中 %1 个已合并归档）").arg(summary.nArchives)
                                : QString());
    }
    ui->labelSummary->setText(sSummary);
}

// 纯计算，不碰界面：工作线程里跑
// （测试也直接调用它，不用起事件循环去等异步结果）。
DlgMergeFile::MergeSummary DlgMergeFile::computeMergeSummary(const QStringList &paths,
                                                            QAtomicInt *pAbort)
{
    MergeSummary summary;
    for (const QString &sPath : paths)
    {
        if (pAbort != nullptr && pAbort->loadAcquire() != 0)
            return summary;
        const QFileInfo fi(sPath);
        ++summary.nInputs;
        if (fi.isDir())
        {
            QDirIterator it(sPath, QDir::Files, QDirIterator::Subdirectories);
            while (it.hasNext())
            {
                if (summary.nFiles >= kSummaryFileCap)
                {
                    summary.bCapped = true;   // 超大目录：后面的不再数
                    break;
                }
                it.next();
                ++summary.nFiles;
                summary.nBytes += it.fileInfo().size();
                // 大目录也可能要走几秒，定期看有没有被取消
                if ((summary.nFiles & 0x3FF) == 0 && pAbort != nullptr
                    && pAbort->loadAcquire() != 0)
                    return summary;
            }
        }
        else if (fi.isFile())
        {
            ++summary.nFiles;
            summary.nBytes += fi.size();
            // 只用于"其中 N 个是已合并归档"的摘要提示
            if (FwArc::hasArchiveSuffix(sPath))
                ++summary.nArchives;
        }
    }
    return summary;
}

void DlgMergeFile::addDroppedUrls(const QList<QUrl> &urls)
{
    bool bAny = false;
    for (const QUrl &u : urls)
    {
        if (!u.isLocalFile())
            continue;
        addPathItem(QFileInfo(u.toLocalFile()).absoluteFilePath(), false);
        bAny = true;
    }
    if (bAny)
        updateSummary();
}

bool DlgMergeFile::eventFilter(QObject *obj, QEvent *ev)
{
    if (obj != ui->treeView->viewport() && obj != ui->treeView)
        return QDialog::eventFilter(obj, ev);

    // 视口尺寸变化（对话框缩放）时重新居中空状态提示
    if (obj == ui->treeView->viewport() && ev->type() == QEvent::Resize)
        updateDropHint();

    switch (ev->type())
    {
    case QEvent::DragEnter:
    case QEvent::DragMove:
    {
        auto *pDrag = static_cast<QDragMoveEvent *>(ev);
        if (pDrag->mimeData() && pDrag->mimeData()->hasUrls())
        {
            // 只接受复制：若接受 MoveAction，Explorer 会删除源文件，
            // 而本对话框只是记录路径、并没有搬运任何字节。
            pDrag->setDropAction(Qt::CopyAction);
            pDrag->accept();
            return true;
        }
        break;
    }
    case QEvent::Drop:
    {
        auto *pDrop = static_cast<QDropEvent *>(ev);
        if (pDrop->mimeData() && pDrop->mimeData()->hasUrls())
        {
            addDroppedUrls(pDrop->mimeData()->urls());
            pDrop->setDropAction(Qt::CopyAction);
            pDrop->accept();
            return true;
        }
        break;
    }
    default:
        break;
    }
    return QDialog::eventFilter(obj, ev);
}

void DlgMergeFile::on_btnAddDir_clicked()
{
    QString sDir = QFileDialog::getExistingDirectory(this, tr("选择待合并的文件"), "", QFileDialog::DontResolveSymlinks);
    addPathItem(sDir);
}

void DlgMergeFile::on_btnAddFile_clicked()
{
    QStringList qLFilePaths = QFileDialog::getOpenFileNames(this, tr("选择待合并的文件"), "", tr("All Files (*.*)"));
    for (const QString &sFilePath : qLFilePaths)
        addPathItem(sFilePath, false);
    if (!qLFilePaths.isEmpty())
        updateSummary();
}

void DlgMergeFile::on_btnDelete_clicked()
{
    // 删除当前选中的项（支持 Ctrl/Shift 多选）。先收集行号，再倒序删除，
    // 避免删前面的行导致后面的行号错位。
    QModelIndexList selected = ui->treeView->selectionModel()->selectedRows();
    if (selected.isEmpty())
    {
        // 有些平台/交互下 selectedRows() 为空但 selectedIndexes() 不为空（单元格选择）
        selected = ui->treeView->selectionModel()->selectedIndexes();
    }
    if (selected.isEmpty())
    {
        QMessageBox::information(this, tr("提示"), tr("请先在列表中选择要删除的文件（可多选）。"));
        return;
    }

    QList<int> rows;
    for (const QModelIndex &idx : selected)
    {
        if (idx.isValid()) rows << idx.row();
    }
    std::sort(rows.begin(), rows.end(), std::greater<int>());
    rows.erase(std::unique(rows.begin(), rows.end()), rows.end());
    for (int row : rows)
        m_hModelFilePath->removeRow(row);
    updateSummary();
}

void DlgMergeFile::on_btnMergedFilePath_clicked()
{
    QString sFilePath = QFileDialog::getSaveFileName(this,
                                                tr("合并后的文件路径"),
                                                "",
                                                FwArc::fileDialogFilter());
    if(sFilePath.isEmpty())
        return;

    ui->leMergedFilePath->setText(sFilePath);
}

void DlgMergeFile::on_btnOk_clicked()
{
    if(m_hModelFilePath->rowCount() == 0)
    {
        QMessageBox::information(this, tr("提示"), tr("未选择待合并的文件"));
        return;
    }

    QString sOutput = ui->leMergedFilePath->text().trimmed();
    if(sOutput.isEmpty())
    {
        QMessageBox::information(this, tr("提示"), tr("合并后的文件路径为空"));
        return;
    }

    // 目录 / 与输入冲突：与输入框下方的实时提示共用同一份判断
    const QString sProblem = outputPathProblem(sOutput);
    if (!sProblem.isEmpty())
    {
        QMessageBox::warning(this, tr("提示"), sProblem);
        return;
    }

    // 没有扩展名（例如只填了 "a"）：补 .fwda 前必须问一次。
    // 静默改名会让「保存对话框里看到的名字」和「磁盘上的名字」对不上，
    // 但直接落成无后缀文件，程序自己的 *.fwda 过滤器又找不到它。
    if (QFileInfo(sOutput).suffix().isEmpty())
    {
        QMessageBox box(QMessageBox::Question, tr("合并文件"),
                        tr("输出文件名没有扩展名：\n%1\n\n"
                           "归档文件统一用 .fwda 后缀，是否追加？").arg(sOutput),
                        QMessageBox::NoButton, this);
        QPushButton *pAppend = box.addButton(tr("追加 .fwda"), QMessageBox::AcceptRole);
        QPushButton *pKeep   = box.addButton(tr("保持原名"), QMessageBox::ActionRole);
        box.addButton(tr("取消"), QMessageBox::RejectRole);
        box.setDefaultButton(pAppend);
        box.exec();

        if (box.clickedButton() == pAppend)
            sOutput = ensureArchiveSuffix(sOutput);
        else if (box.clickedButton() != pKeep)
            return;                       // 取消：不改路径，也不关对话框

        ui->leMergedFilePath->setText(sOutput);   // 先让用户看到最终文件名
    }

    // 相对路径一律按进程当前目录解析，并立刻回显成绝对路径：
    // 否则用户填 "a" 后文件落在启动目录（从 IDE 运行时是 build/.../debug），
    // 合并提示里只有文件数，根本不知道去哪找。
    const QString sFinal = QDir::cleanPath(QFileInfo(sOutput).absoluteFilePath());
    ui->leMergedFilePath->setText(sFinal);

    if (QFileInfo::exists(sFinal))
    {
        if (QMessageBox::question(this, tr("提示"),
                tr("输出文件已存在，合并后会替换它：\n%1\n\n是否继续？").arg(sFinal),
                QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes)
            return;
    }
    accept();
}

void DlgMergeFile::on_btnCancel_clicked()
{
    reject();
}

bool DlgMergeFile::duplicatCheck(QString sFilePath)
{
    int nRowCount = m_hModelFilePath->rowCount();
    for(int i = 0; i < nRowCount; i++)
    {
        QString sItemText = m_hModelFilePath->item(i, 0)->text();
        if(QString::compare(sItemText, sFilePath) == 0)
            return false;
    }
    return true;
}
