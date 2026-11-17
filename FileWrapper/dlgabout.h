#ifndef DLGABOUT_H
#define DLGABOUT_H

#include <QDialog>

namespace Ui {
class DlgAbout;
}

class DlgAbout : public QDialog
{
    Q_OBJECT

public:
    explicit DlgAbout(QWidget *parent = nullptr);
    ~DlgAbout();

    // 追加运行信息（版本、构建时间、已加载插件数等）
    void setExtraInfo(const QString &sHtml);

private:
    Ui::DlgAbout *ui;
};

#endif // DLGABOUT_H
