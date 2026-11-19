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
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QMimeData>
#include <QUrl>
#include "archiveformat.h"

#include <algorithm>
#include <functional>
#include "icons.h"

DlgMergeFile::DlgMergeFile(QWidget *parent) :
    QDialog(parent),
    ui(new Ui::DlgMergeFile)
{
    ui->setupUi(this);
    ui->btnAddFile->setIcon(FwIcons::get(QStringLiteral("add")));
    ui->btnDelete->setIcon(FwIcons::get(QStringLiteral("delete")));
    ui->btnAddDir->setIcon(FwIcons::get(QStringLiteral("add_dir")));
    ui->btnMergedFilePath->setIcon(FwIcons::get(QStringLiteral("browse")));
    ui->btnOk->setIcon(FwIcons::get(QStringLiteral("ok")));
    ui->btnCancel->setIcon(FwIcons::get(QStringLiteral("cancel")));

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
    m_hModelFilePath->setHeaderData(0, Qt::Horizontal, tr("文件路径"));
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

    updateSummary();
    updateButtonStates();
    updateResolvedPath();

    // 解析后的路径提示用内联样式：灰=正常，红=不可用（颜色由 updateResolvedPath 切换）
    ui->labelResolvedPath->setStyleSheet(
        QStringLiteral("QLabel#labelResolvedPath { color: #6B7280; }"));
}

DlgMergeFile::~DlgMergeFile()
{
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
//   - 程序自己的「加载文件」对话框默认只看 *.fwda / *.dat，找不到它；
//   - 任何按后缀关联的文件管理器都打不开它。
// 已有扩展名的（包括 "v1.2" 这种用点分隔的版本号、以及老的 .dat）一律不动，
// 避免改坏用户意图。
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

// 摘要：输入项数、展开后的文件数/总大小、其中多少个是已合并的 .dat
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
void DlgMergeFile::updateResolvedPath()
{
    if (ui->labelResolvedPath == nullptr)
        return;

    const QString sText = ui->leMergedFilePath->text().trimmed();
    if (sText.isEmpty())
    {
        ui->labelResolvedPath->clear();
        ui->labelResolvedPath->setToolTip(QString());
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

void DlgMergeFile::updateSummary()
{
    int nInputs = 0, nFiles = 0, nArchives = 0;
    qint64 nBytes = 0;
    const int nCap = 200000;   // 超大目录防止统计本身卡住界面
    for (int i = 0; i < m_hModelFilePath->rowCount(); ++i)
    {
        const QString sPath = m_hModelFilePath->item(i, 0)->text();
        const QFileInfo fi(sPath);
        ++nInputs;
        if (fi.isDir())
        {
            QDirIterator it(sPath, QDir::Files, QDirIterator::Subdirectories);
            while (it.hasNext() && nFiles < nCap)
            {
                it.next();
                ++nFiles;
                nBytes += it.fileInfo().size();
            }
        }
        else if (fi.isFile())
        {
            ++nFiles;
            nBytes += fi.size();
            // 只用于"其中 N 个是已合并归档"的摘要提示：新旧后缀都算
            if (FwArc::hasArchiveSuffix(sPath))
                ++nArchives;
        }
    }

    // 摘要单独一行（标题下面），不要拼进「待合并的文件路径」里：
    // 统计文字会随添加的目录变化，拼在标题里会不断改标题宽度、把右侧文件树挤得左右跳。
    QString sSummary;
    if (nInputs > 0)
    {
        sSummary = tr("%1 个输入，约 %2 个文件，%3%4")
                       .arg(nInputs)
                       .arg(nFiles >= nCap ? QStringLiteral(">") + QString::number(nCap)
                                           : QString::number(nFiles))
                       .arg(FwArc::formatSize(nBytes))
                       .arg(nArchives > 0
                                ? tr("（其中 %1 个已合并归档）").arg(nArchives)
                                : QString());
    }
    ui->labelSummary->setText(sSummary);
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
    // 但直接落成无后缀文件，程序自己的 *.fwda / *.dat 过滤器又找不到它。
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
