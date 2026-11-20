#ifndef PLUGININTERFACE_H
#define PLUGININTERFACE_H

#include <QByteArray>
#include <QFile>
#include <QFileInfo>
#include <QObject>
#include <QString>
#include <QWidget>

#include <utility>

// ============================================================================
// 插件 API 版本（B1：版本协商）
//
// **每当 PluginInterface 的虚函数表发生变化（增/删虚函数、改签名）都必须 +1**，
// 并同步到所有插件 JSON 里的 "apiVersion"。
//
// 为什么需要：主程序与插件是两个二进制，各自编译。接口从最初的 2 个纯虚一路长到
// 现在 14 个虚函数（另有虚析构），每次增加虚函数都是一次 ABI 断裂 —— 「旧插件 DLL + 新主程序」时，
// 主程序调用新虚函数会越界访问旧插件的虚函数表（UB/崩溃），而 qobject_cast 仍会成功，
// 表现为"能加载、一用就崩"。版本不一致时，编译器/链接器都不会报错。
//
// 现在的做法：主程序在**实例化插件之前**先读它的内嵌元数据（Q_PLUGIN_METADATA
// 里的 JSON），比对 "apiVersion"；不匹配就拒绝加载并给出说明。实例化之后再用
// apiVersion() 复核一次，防止元数据与实际代码不一致。
//
// 版本史：
//   1  最初形态（sendFileData / getPluginWidget）
//   2  增加 stopPlayback / hasBackgroundPlayback / createInstance /
//      prefersFileByPath / sendFileByPath / setSourceName（C1/B1 时定版）
//   3  C2：新增 sendFile(const PluginFile &)，统一"内存 / 路径"两种数据载体
//   4  新增 sendFileMapped(char*, qint64, name)：归档内容区只读内存映射，
//      媒体插件播放期间直接引用、零拷贝（见 PluginInterface::sendFileMapped）
//   5  新增 stopPlaybackAndWait()：停止后台线程并确认可安全 unmap（M-04）
//   6  新增 lastError()：打开失败时插件回报**真实原因**（例如"没有可用的音频
//      输出设备"），取代宿主统一显示的"插件无法显示该文件"（见 lastError）
//   7  新增 isPlaybackStopped()：停播超时后宿主可非阻塞地复查后台线程是否已
//      自行退出，从而解除 m_setStopUnsafe，恢复保存/重载（见 isPlaybackStopped）
// ============================================================================
#define PLUGIN_API_VERSION 7

// ==================== 文件数据载体（C2） ====================
//
// 历史通道 sendFileData(char*, qint64) 只有"一整块可写内存"一种形态，导致：
//   * 主程序必须先把文件读成 char[]，插件再复制成自己的 QByteArray（每次两份）；
//   * 大文件只能靠"抽到临时文件 + 另一个接口"绕开，凭据是 128 MB 阈值，
//     插件还要额外实现 prefersFileByPath()/sendFileByPath() 一套。
//
// PluginFile 把「数据」抽象成 内存块 或 文件路径 两种（同一个类、不做虚函数，
// 因此跨 DLL 传递没有额外 ABI 负担），主程序构造好交给 PluginInterface::sendFile()：
//   * 只吃字节的插件（Text/Picture/Book/Word…）直接用 memory()，不再复制；
//   * 想自己流式读的插件（PDF/媒体…）看 isPath()/path()；
//   * 还没迁移的插件走 sendFile() 的默认实现，行为与旧版完全一致（见下）。
//
// 注意 memory() 返回的是 const 引用：PluginFile 的生命周期只有本次 sendFile 调用，
// 插件若要长期持有必须自己复制（这正是旧行为）。
class PluginFile
{
public:
    enum class Kind { Memory, Path };

    PluginFile() = default;

    static PluginFile fromMemory(QByteArray data, const QString &displayName = QString())
    {
        PluginFile f;
        f.m_kind = Kind::Memory;
        f.m_data = std::move(data);
        f.m_displayName = displayName;
        return f;
    }

    static PluginFile fromPath(const QString &path, const QString &displayName = QString())
    {
        PluginFile f;
        f.m_kind = Kind::Path;
        f.m_path = path;
        f.m_displayName = displayName;
        return f;
    }

    Kind kind() const { return m_kind; }
    bool isMemory() const { return m_kind == Kind::Memory; }
    bool isPath()   const { return m_kind == Kind::Path; }
    QString displayName() const { return m_displayName; }

    // 仅 Memory：块本身（不复制）
    const QByteArray &memory() const { return m_data; }
    // 仅 Path：文件路径（可能为空）
    QString path() const { return m_path; }

    // 大小：内存块为字节数；路径为文件大小（读不到时 -1）
    qint64 size() const
    {
        if (isMemory()) return m_data.size();
        return m_path.isEmpty() ? -1 : QFileInfo(m_path).size();
    }

    // 取全部字节：Memory 直接返回引用副本；Path 读文件（失败返回空）。
    QByteArray readAll() const
    {
        if (isMemory()) return m_data;
        QFile f(m_path);
        if (!f.open(QIODevice::ReadOnly)) return QByteArray();
        return f.readAll();
    }

    bool isEmpty() const { return size() <= 0; }

private:
    Kind    m_kind = Kind::Memory;
    QByteArray m_data;
    QString m_path;
    QString m_displayName;
};

// ==================== 插件接口 ====================
class PluginInterface
{
public:
    virtual ~PluginInterface(){}

    // 接口版本（编译期内嵌）：插件必须返回 PLUGIN_API_VERSION。
    // 注意这是纯虚函数 —— 新增插件若忘了实现，编译期就会报错，而不是运行期才崩。
    virtual int apiVersion() const = 0;

    // 旧的数据通道（v1）：一整块可写内存。
    // 仍然保留：老插件、以及需要"就地改写缓冲区"的插件继续用它。
    virtual void sendFileData(char *szFileData, qint64 nFileLen) = 0;

    virtual QWidget* getPluginWidget() = 0;

    // —— C2：统一数据通道（v3）——
    // 主程序现在优先调用这个；默认实现保持旧插件行为完全不变：
    //   * 路径 + 插件声明 prefersFileByPath() -> sendFileByPath()
    //   * 其它情况读出字节 -> sendFileData()（兼容层，会复制一次，和旧版一致）
    // 想省掉这次复制的插件只需重写本函数，直接用 file.memory()。
    // 返回值：true 表示已处理；false 表示放弃（主程序可回退/报错）。
    virtual bool sendFile(const PluginFile &file)
    {
        if (file.isPath() && prefersFileByPath())
            return sendFileByPath(file.path(), file.displayName());

        QByteArray owned;
        if (file.isMemory()) owned = file.memory();
        else                 owned = file.readAll();

        if (owned.isEmpty())
        {
            sendFileData(nullptr, 0);
            return false;
        }
        sendFileData(owned.data(), owned.size());
        return true;
    }

    // —— v4：归档内存映射通道（零拷贝流式读取）——
    // 主程序把归档中该条目的内容区 mmap 成只读内存后调用本函数。
    // 返回 true 表示插件已在后台播放中直接引用这段内存（宿主必须保持映射有效，
    // 直到 stopPlayback()/换文件/实例销毁）；返回 false 表示不支持，
    // 宿主会回退到"抽取临时文件"或"整块读入内存"。
    // 默认返回 false，旧插件无需改动。
    virtual bool sendFileMapped(char *pData, qint64 nFileLen, const QString &displayName)
    {
        Q_UNUSED(pData);
        Q_UNUSED(nFileLen);
        Q_UNUSED(displayName);
        return false;
    }

    // 切换文件时由主程序调用：停止该插件正在进行的后台播放（视频/音频等）。
    virtual void stopPlayback() {}
    // 停止并等待后台线程真正退出；返回 false 表示仍可能访问宿主映射，
    // 宿主不得 unmap。默认实现等价于旧 stopPlayback()（同步插件恒为 true）。
    virtual bool stopPlaybackAndWait() { stopPlayback(); return true; }
    // 该插件是否存在「后台播放」（音视频解码线程 / 音频输出等持续占用的运行态）。
    virtual bool hasBackgroundPlayback() const { return false; }

    // —— v7：停播超时后的非阻塞复查 ——
    // stopPlaybackAndWait() 返回 false 后，宿主会把实例记为 unsafe 并禁止
    // unmap/delete/卸载 DLL。但后台线程可能过一会儿才真正退出；宿主需要一个
    // **不阻塞 GUI** 的查询来解除这个状态。默认实现按 hasBackgroundPlayback()
    // 保守判断：没有后台播放的插件恒为 true；声称有后台播放却没重写本函数的
    // 插件返回 false（宁可继续保留，也不误放行）。
    // 约定：只有确认此刻起不会再访问宿主的映射/临时文件时才能返回 true。
    virtual bool isPlaybackStopped() const { return !hasBackgroundPlayback(); }

    // 由主程序在「新建标签页 / 在该标签页打开文件」时调用：创建独立的插件实例。
    // 必须实现（B2）：返回空会被主程序当作"打开失败"，不允许退化成共享实例。
    virtual PluginInterface* createInstance() { return nullptr; }

    // —— 以下为可选扩展，默认实现保持旧插件行为完全不变 ——

    // 大文件（>= 阈值）时主程序把归档内容流式抽取到临时文件，优先走此接口，
    // 避免把数 GB 数据一次性读进内存再复制一份（P1-2）。
    // 返回 true 表示插件已自行处理该文件；返回 false 时主程序回退到内存路径。
    // 注意：path 指向的文件在本次会话结束前一直有效（Plugins 可在后台线程持续读取）。
    virtual bool prefersFileByPath() const { return false; }
    virtual bool sendFileByPath(const QString &path, const QString &displayName)
    {
        Q_UNUSED(path); Q_UNUSED(displayName);
        return false;
    }

    // 主程序在送数据前告知文件名，插件可据此做标题回退（P2-7）
    virtual void setSourceName(const QString &name) { Q_UNUSED(name); }

    // —— v6：失败原因回报 ——
    // sendFile() / sendFileByPath() / sendFileMapped() 返回 false 之后，宿主用
    // 它把**真实原因**呈现给用户。以前宿主只能显示「插件无法显示该文件：xxx」，
    // 于是"没有音频输出设备""解码器不支持该编码"和"文件损坏"在界面上完全一样。
    //
    // 约定：
    //   * 返回空串 = 插件没提供原因，宿主退回通用提示（旧插件行为不变）；
    //   * 返回非空 = 插件**已经自行复位了自己的界面**并在界面上说明了原因，
    //     宿主不要再调 sendFileData(nullptr, 0) 清屏（会把说明一起抹掉）；
    //   * 每次新的打开尝试开始时插件必须清空上一次的失败原因；
    //   * 一句话、不带换行，供状态栏/对话框直接拼接。
    virtual QString lastError() const { return QString(); }
};

#define PluginTemplate_main_iid "FileWrapper.main"

QT_BEGIN_NAMESPACE
Q_DECLARE_INTERFACE(PluginInterface, PluginTemplate_main_iid)
QT_END_NAMESPACE

#endif // PLUGININTERFACE_H
