#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QTextBrowser>
#include <QLabel>
#include <QUrl>
#include <QVariant>
#include <QMap>
#include <QByteArray>
#include <QStringList>
#include <QList>
#include <QDomDocument>

namespace Ui {
class MainWindow;
}

// 自定义文本浏览器：重写 loadResource，使其能从 EPUB/FB2 内嵌数据加载图片
class BookBrowser : public QTextBrowser
{
    Q_OBJECT
public:
    explicit BookBrowser(QWidget *parent = nullptr);
    void setEntries(const QMap<QString, QByteArray> *entries) { m_entries = entries; }
    void setFb2Images(const QMap<QString, QByteArray> *imgs)    { m_fb2Images = imgs; }
    void setCurDir(const QString &dir)                          { m_curDir = dir; }

protected:
    QVariant loadResource(int type, const QUrl &name) override;

private:
    const QMap<QString, QByteArray> *m_entries  = nullptr;
    const QMap<QString, QByteArray> *m_fb2Images = nullptr;
    QString m_curDir;
};

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow();

    void showBook(char *szBuf, qint64 nFileLen);

private slots:
    void on_actionPrevChapter_triggered();
    void on_actionNextChapter_triggered();
    void on_actionZoomIn_triggered();
    void on_actionZoomOut_triggered();

private:
    void reset();
    void loadChapter(int idx);
    void showMessage(const QString &msg);

    QString detectFormat(const QByteArray &data) const;
    bool    loadEpub(const QByteArray &data);
    bool    loadFb2(const QByteArray &data);
    bool    loadMobi(const QByteArray &data);   // 依赖 libmobi（WITH_LIBMOBI）
    QString fb2ToHtml(const QDomNode &node);

    // 轻量 XML 解析辅助（避免命名空间带来的 QDomDocument 复杂性）
    QList<QMap<QString, QString>> findTags(const QString &xml, const QString &tag) const;
    QString findTagAttr(const QString &xml, const QString &tag, const QString &name) const;
    QString textOfTag(const QString &xml, const QString &tag) const;

    Ui::MainWindow *ui;
    BookBrowser     *m_browser    = nullptr;
    QLabel          *m_infoLabel  = nullptr;

    QByteArray                   m_bookData;          // 持有原始数据副本，避免主程序释放后 use-after-free
    QMap<QString, QByteArray>    m_entries;           // zip 内路径 -> 字节（EPUB / FB2-zip）
    QMap<QString, QByteArray>    m_fb2Images;         // FB2 binary id -> 字节
    QStringList                  m_chapterHtml;       // 每章渲染后的 HTML
    QStringList                  m_chapterDirs;       // 每章在 zip 中的目录（用于图片相对路径解析）
    QStringList                  m_chapterTitles;     // 每章标题
    int                          m_curChapter = 0;
    int                          m_zoomSteps  = 0;    // 累计缩放步数，切章后重新应用
    QString                      m_bookTitle;
};

#endif // MAINWINDOW_H
