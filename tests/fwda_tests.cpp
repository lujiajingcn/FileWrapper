// ============================================================
// FWDA v5 端到端测试：真实 FileManager + 加密归档
//
// 不依赖插件 / GUI 交互；只验证“合并 → 打开 → 读取 → 导出 → 保存”链路。
// 使用进程内测试密钥，不触碰用户 QSettings。
// ============================================================

#include "filemanager.h"
#include "dlgsettings.h"
#include "crypto/argonderive.h"
#include "crypto/keyring.h"

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QSettings>
#include <QTemporaryDir>

#include <cstdio>

namespace {

int g_fail = 0;

void check(bool ok, const char *psName)
{
    std::printf("%s %s\n", ok ? "[ OK ]" : "[FAIL]", psName);
    if (!ok) ++g_fail;
}

bool writeFile(const QString &path, const QByteArray &data)
{
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    return f.write(data) == data.size();
}

QByteArray readFile(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return QByteArray();
    return f.readAll();
}

QString findEntry(FileManager &fm, const QString &name)
{
    for (const FILEINFO &fi : fm.getFileInfos())
        if (fi.sFileName == name)
            return fi.sFilePath;
    return QString();
}

bool runMerge(FileManager &fm, const QVector<QString> &inputs, const QString &out)
{
    bool done = false, ok = false;
    QObject::connect(&fm, &FileManager::operationFinished,
                     [&done, &ok](bool bOk, const QString &msg) {
                         done = true; ok = bOk;
                         if (!bOk) std::printf("        merge error: %s\n", qPrintable(msg));
                     });
    fm.QMergeFiles(inputs, out);
    return done && ok;
}

} // namespace

int main(int argc, char **argv)
{
    qputenv("QT_QPA_PLATFORM", QByteArray("offscreen"));
    QApplication app(argc, argv);

    QByteArray productKey;
    QString keyErr;

    // 受限沙箱里系统临时目录可能不可写：工作目录放在当前可写目录下。
    const QString sDir = QDir::currentPath() + QStringLiteral("/work");
    QDir(sDir).removeRecursively();
    check(QDir().mkpath(sDir), "创建临时目录");

    // 密钥环存储隔离：QSettings 指到工作目录，绝不改用户真实配置。
    const QString sSettings = sDir + QStringLiteral("/settings");
    QDir().mkpath(sSettings);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, sSettings);

    // —— 密钥环：配置 / 去重 / 历史 / 删除 / 持久化（DPAPI）——
    {
        auto &ring = FwCrypto::KeyRing::instance();
        ring.reloadFromStorage();
        check(!ring.hasKey(), "初始未配置密钥");

        check(ring.setCurrentKey(QStringLiteral("FWDA-TEST-VECTOR"), &keyErr),
              "配置密钥字符串");
        check(ring.hasKey() && ring.currentFingerprint().size() == 8, "密钥指纹");
        check(ring.setCurrentKey(QStringLiteral("FWDA-TEST-VECTOR"), &keyErr)
                  && ring.fingerprints().size() == 1,
              "重复应用同一字符串不新增条目");
        check(ring.setCurrentKey(QStringLiteral("FWDA-SECOND-KEY"), &keyErr)
                  && ring.fingerprints().size() == 2,
              "新字符串进入历史密钥");
        check(ring.removeCurrentKey(&keyErr) && ring.fingerprints().size() == 1,
              "删除当前密钥后历史生效");

        ring.reloadFromStorage();
        check(ring.hasKey() && ring.fingerprints().size() == 1,
              "密钥环持久化（DPAPI 往返）");

        ring.clearAllKeys();
        check(!ring.hasKey(), "清空全部密钥");
    }

    // —— 设置对话框冒烟：能构造、能显示、能读密钥环状态 ——
    {
        DlgSettings dlg;
        dlg.show();
        QApplication::processEvents();
        check(dlg.isVisible(), "设置对话框可打开");
        dlg.close();
    }

    // —— 测试密钥（进程内，不落盘）——
    check(FwCrypto::deriveProductKey(QStringLiteral("FWDA-TEST-VECTOR"), &productKey, &keyErr),
          "派生测试密钥");
    FwCrypto::KeyRing::instance().setTestKey(productKey);

    const QByteArray contentA = QByteArray("hello-fwda-plaintext-marker-1234567890");
    QByteArray contentB(1500 * 1024, 'x');
    for (int i = 0; i < contentB.size(); ++i)
        contentB[i] = char((i * 31 + 7) & 0xFF);

    const QString fileA = sDir + "/a.txt";
    const QString fileB = sDir + "/b.bin";
    check(writeFile(fileA, contentA), "写入 a.txt");
    check(writeFile(fileB, contentB), "写入 b.bin");

    const QString archive = sDir + "/out.fwda";
    FileManager fmMerge;
    check(runMerge(fmMerge, { fileA, fileB }, archive), "合并生成加密归档");

    // —— 文件里不能出现明文/文件名 ——
    const QByteArray raw = readFile(archive);
    check(raw.startsWith("FWDA"), "魔数正确");
    check(!raw.contains("hello-fwda-plaintext-marker"), "归档中无明文标记");
    check(!raw.contains("a.txt"), "归档中无文件名");

    // —— 打开与读取 ——
    FileManager fmLoad;
    check(fmLoad.QLoadMergedFile(archive), "加载归档");
    const QString pathA = findEntry(fmLoad, "a.txt");
    const QString pathB = findEntry(fmLoad, "b.bin");
    check(!pathA.isEmpty() && !pathB.isEmpty(), "条目可见");
    check(fmLoad.entryContent(pathA, &keyErr) == contentA, "读取 a.txt 内容");
    check(fmLoad.entryContent(pathB, &keyErr) == contentB, "读取 b.bin 内容（跨块）");

    QString report;
    check(fmLoad.verifyArchive(&report), "内容校验通过");

    // —— 导出 ——
    const QString exportDir = sDir + "/export";
    QDir().mkpath(exportDir);
    fmLoad.outputFile(pathA, exportDir);
    check(readFile(exportDir + "/a.txt") == contentA, "导出 a.txt");

    // —— 分割 ——
    const QString splitDir = sDir + "/split";
    QDir().mkpath(splitDir);
    fmLoad.QSplitFiles(archive, false, splitDir);
    check(readFile(splitDir + "/a.txt") == contentA, "分割 a.txt");
    check(readFile(splitDir + "/b.bin") == contentB, "分割 b.bin");

    // 归档还要被后续保存重写：先释放句柄（真实程序同一时刻只有一个 FileManager）。
    fmLoad.unLoadMergedFile();

    // —— 错误密钥必须拒绝 ——
    FwCrypto::KeyRing::instance().setTestKey(QByteArray(32, 'Q'));
    FileManager fmWrong;
    check(!fmWrong.QLoadMergedFile(archive), "错误密钥被拒绝");
    FwCrypto::KeyRing::instance().setTestKey(productKey);

    // —— 追加保存（新增文件）——
    FileManager fmSave;
    check(fmSave.QLoadMergedFile(archive), "重新加载（保存前）");
    const QByteArray contentC(".new-file-after-load");
    const QString fileC = sDir + "/c.txt";
    check(writeFile(fileC, contentC), "写入 c.txt");
    fmSave.addFiles({ fileC });
    check(fmSave.save(), "追加保存");
    fmSave.unLoadMergedFile();
    {
        FileManager fmReload;
        check(fmReload.QLoadMergedFile(archive), "追加后重新加载");
        const QString pathC = findEntry(fmReload, "c.txt");
        check(!pathC.isEmpty() && fmReload.entryContent(pathC, &keyErr) == contentC,
              "新增条目内容正确");
        check(fmReload.entryContent(findEntry(fmReload, "a.txt"), &keyErr) == contentA,
              "旧条目内容不受影响");
    }

    // —— 只改头部保存（删除条目）——
    FileManager fmDelete;
    check(fmDelete.QLoadMergedFile(archive), "重新加载（删除前）");
    const QString pathB2 = findEntry(fmDelete, "b.bin");
    check(fmDelete.deleteFile(pathB2), "删除 b.bin");
    check(fmDelete.save(), "删除后保存（只改头部）");
    fmDelete.unLoadMergedFile();
    {
        FileManager fmReload;
        check(fmReload.QLoadMergedFile(archive), "删除后重新加载");
        check(findEntry(fmReload, "b.bin").isEmpty(), "删除生效");
        check(fmReload.entryContent(findEntry(fmReload, "a.txt"), &keyErr) == contentA,
              "删除后剩余条目可读");
    }

    // —— 整理归档 ——
    FileManager fmCompact;
    check(fmCompact.QLoadMergedFile(archive), "重新加载（整理前）");
    check(fmCompact.compactArchive(), "整理归档");
    {
        FileManager fmReload;
        check(fmReload.QLoadMergedFile(archive), "整理后重新加载");
        check(fmReload.entryContent(findEntry(fmReload, "a.txt"), &keyErr) == contentA,
              "整理后内容正确");
    }

    std::printf("\n%s（%d 项失败）\n", g_fail == 0 ? "全部通过" : "存在失败", g_fail);
    return g_fail == 0 ? 0 : 1;
}
