#include "domain/address_table.h"

#include <utility>

namespace hlm {

namespace {

AddressDef def(QString area, quint16 addr, QString name, QString desc,
               AccessType access, ValueType type, quint16 high = 0,
               double min = 0.0, double max = 0.0, double scale = 1.0,
               QString unit = QString())
{
    AddressDef d;
    d.area = std::move(area);
    d.address = addr;
    d.name = std::move(name);
    d.description = std::move(desc);
    d.access = access;
    d.valueType = type;
    d.highWord = high;
    d.min = min;
    d.max = max;
    d.scale = scale;
    d.unit = std::move(unit);
    return d;
}

} // namespace

AddressTable::AddressTable()
{
    // --- M area (coils) -----------------------------------------------------
    // 0-based protocol addresses (M0 -> 0). Source: 需求/PLC上位机地址及要求.txt.
    m_defs.push_back(def(QStringLiteral("M"), 0,  QStringLiteral("M0"),
        QStringLiteral("急停有效"), AccessType::Read, ValueType::U16));
    m_defs.push_back(def(QStringLiteral("M"), 1,  QStringLiteral("M1"),
        QStringLiteral("手动模式"), AccessType::Read, ValueType::U16));
    m_defs.push_back(def(QStringLiteral("M"), 2,  QStringLiteral("M2"),
        QStringLiteral("自动模式"), AccessType::Read, ValueType::U16));
    m_defs.push_back(def(QStringLiteral("M"), 3,  QStringLiteral("M3"),
        QStringLiteral("运行状态"), AccessType::Read, ValueType::U16));
    m_defs.push_back(def(QStringLiteral("M"), 4,  QStringLiteral("M4"),
        QStringLiteral("实时故障"), AccessType::Read, ValueType::U16));
    m_defs.push_back(def(QStringLiteral("M"), 5,  QStringLiteral("M5"),
        QStringLiteral("安全门打开"), AccessType::Read, ValueType::U16));
    m_defs.push_back(def(QStringLiteral("M"), 6,  QStringLiteral("M6"),
        QStringLiteral("安全光栅遮挡"), AccessType::Read, ValueType::U16));
    m_defs.push_back(def(QStringLiteral("M"), 7,  QStringLiteral("M7"),
        QStringLiteral("气压低"), AccessType::Read, ValueType::U16));
    m_defs.push_back(def(QStringLiteral("M"), 8,  QStringLiteral("M8"),
        QStringLiteral("自动准备完成映射(M60)"), AccessType::Read, ValueType::U16));
    m_defs.push_back(def(QStringLiteral("M"), 9,  QStringLiteral("M9"),
        QStringLiteral("回原点完成映射(M61)"), AccessType::Read, ValueType::U16));
    m_defs.push_back(def(QStringLiteral("M"), 10, QStringLiteral("M10"),
        QStringLiteral("自动挡停伸出命令"), AccessType::Read, ValueType::U16));
    m_defs.push_back(def(QStringLiteral("M"), 11, QStringLiteral("M11"),
        QStringLiteral("相机触发中"), AccessType::Read, ValueType::U16));
    m_defs.push_back(def(QStringLiteral("M"), 12, QStringLiteral("M12"),
        QStringLiteral("拍照超时标志"), AccessType::Read, ValueType::U16));
    m_defs.push_back(def(QStringLiteral("M"), 13, QStringLiteral("M13"),
        QStringLiteral("直通模式"), AccessType::Read, ValueType::U16));
    m_defs.push_back(def(QStringLiteral("M"), 14, QStringLiteral("M14"),
        QStringLiteral("故障锁存"), AccessType::Read, ValueType::U16));
    m_defs.push_back(def(QStringLiteral("M"), 30, QStringLiteral("M30"),
        QStringLiteral("手动皮带点动命令"), AccessType::Read, ValueType::U16));
    m_defs.push_back(def(QStringLiteral("M"), 31, QStringLiteral("M31"),
        QStringLiteral("手动调宽正转命令"), AccessType::Read, ValueType::U16));
    m_defs.push_back(def(QStringLiteral("M"), 32, QStringLiteral("M32"),
        QStringLiteral("手动调宽反转命令"), AccessType::Read, ValueType::U16));
    m_defs.push_back(def(QStringLiteral("M"), 33, QStringLiteral("M33"),
        QStringLiteral("手动挡停命令"), AccessType::Read, ValueType::U16));
    m_defs.push_back(def(QStringLiteral("M"), 34, QStringLiteral("M34"),
        QStringLiteral("自动调宽运行标志"), AccessType::Read, ValueType::U16));
    m_defs.push_back(def(QStringLiteral("M"), 35, QStringLiteral("M35"),
        QStringLiteral("调宽方向标志"), AccessType::Read, ValueType::U16));
    m_defs.push_back(def(QStringLiteral("M"), 40, QStringLiteral("M40"),
        QStringLiteral("自动流程皮带运行命令"), AccessType::Read, ValueType::U16));
    m_defs.push_back(def(QStringLiteral("M"), 41, QStringLiteral("M41"),
        QStringLiteral("皮带输出汇总"), AccessType::Read, ValueType::U16));
    m_defs.push_back(def(QStringLiteral("M"), 42, QStringLiteral("M42"),
        QStringLiteral("皮带自动常转命令"), AccessType::ReadWrite, ValueType::U16));
    m_defs.push_back(def(QStringLiteral("M"), 50, QStringLiteral("M50"),
        QStringLiteral("回原点启动标志"), AccessType::Read, ValueType::U16));
    m_defs.push_back(def(QStringLiteral("M"), 51, QStringLiteral("M51"),
        QStringLiteral("回原点阶段1反转"), AccessType::Read, ValueType::U16));
    m_defs.push_back(def(QStringLiteral("M"), 52, QStringLiteral("M52"),
        QStringLiteral("回原点阶段2正转"), AccessType::Read, ValueType::U16));
    m_defs.push_back(def(QStringLiteral("M"), 53, QStringLiteral("M53"),
        QStringLiteral("回原点下降沿辅助"), AccessType::Read, ValueType::U16));
    m_defs.push_back(def(QStringLiteral("M"), 60, QStringLiteral("M60"),
        QStringLiteral("自动准备完成"), AccessType::Read, ValueType::U16));
    m_defs.push_back(def(QStringLiteral("M"), 61, QStringLiteral("M61"),
        QStringLiteral("回原点完成标志"), AccessType::Read, ValueType::U16));
    m_defs.push_back(def(QStringLiteral("M"), 100, QStringLiteral("M100"),
        QStringLiteral("上位机急停请求"), AccessType::ReadWrite, ValueType::U16));
    m_defs.push_back(def(QStringLiteral("M"), 101, QStringLiteral("M101"),
        QStringLiteral("上位机启动"), AccessType::ReadWrite, ValueType::U16));
    m_defs.push_back(def(QStringLiteral("M"), 102, QStringLiteral("M102"),
        QStringLiteral("上位机停止"), AccessType::ReadWrite, ValueType::U16));
    m_defs.push_back(def(QStringLiteral("M"), 103, QStringLiteral("M103"),
        QStringLiteral("上位机复位"), AccessType::ReadWrite, ValueType::U16));
    m_defs.push_back(def(QStringLiteral("M"), 104, QStringLiteral("M104"),
        QStringLiteral("自动模式"), AccessType::ReadWrite, ValueType::U16));
    m_defs.push_back(def(QStringLiteral("M"), 105, QStringLiteral("M105"),
        QStringLiteral("直通模式"), AccessType::ReadWrite, ValueType::U16));
    m_defs.push_back(def(QStringLiteral("M"), 106, QStringLiteral("M106"),
        QStringLiteral("手动调宽正转"), AccessType::ReadWrite, ValueType::U16));
    m_defs.push_back(def(QStringLiteral("M"), 107, QStringLiteral("M107"),
        QStringLiteral("手动调宽反转"), AccessType::ReadWrite, ValueType::U16));
    m_defs.push_back(def(QStringLiteral("M"), 108, QStringLiteral("M108"),
        QStringLiteral("手动皮带点动"), AccessType::ReadWrite, ValueType::U16));
    m_defs.push_back(def(QStringLiteral("M"), 109, QStringLiteral("M109"),
        QStringLiteral("手动挡停"), AccessType::ReadWrite, ValueType::U16));
    m_defs.push_back(def(QStringLiteral("M"), 110, QStringLiteral("M110"),
        QStringLiteral("光栅屏蔽"), AccessType::ReadWrite, ValueType::U16));
    m_defs.push_back(def(QStringLiteral("M"), 111, QStringLiteral("M111"),
        QStringLiteral("门磁屏蔽"), AccessType::ReadWrite, ValueType::U16));
    m_defs.push_back(def(QStringLiteral("M"), 112, QStringLiteral("M112"),
        QStringLiteral("HMI看门狗"), AccessType::ReadWrite, ValueType::U16));

    // --- D area (holding registers) -----------------------------------------
    // 0-based protocol addresses (D100 -> 100). Source: 需求/PLC上位机地址及要求.txt.
    m_defs.push_back(def(QStringLiteral("D"), 100, QStringLiteral("D100"),
        QStringLiteral("状态字1 M0~M15映射"), AccessType::Read, ValueType::U16));
    m_defs.push_back(def(QStringLiteral("D"), 102, QStringLiteral("D102"),
        QStringLiteral("状态字2 M200~M215映射(流程步骤)"), AccessType::Read, ValueType::U16));
    m_defs.push_back(def(QStringLiteral("D"), 103, QStringLiteral("D103"),
        QStringLiteral("状态字3 M30~M45映射"), AccessType::Read, ValueType::U16));
    m_defs.push_back(def(QStringLiteral("D"), 104, QStringLiteral("D104"),
        QStringLiteral("状态字4 M300~M315映射(输入)"), AccessType::Read, ValueType::U16));
    m_defs.push_back(def(QStringLiteral("D"), 105, QStringLiteral("D105"),
        QStringLiteral("状态字5 M316~M331映射(扩展输入)"), AccessType::Read, ValueType::U16));
    m_defs.push_back(def(QStringLiteral("D"), 110, QStringLiteral("D110"),
        QStringLiteral("故障代码"), AccessType::Read, ValueType::U16, 0, 0, 10));
    m_defs.push_back(def(QStringLiteral("D"), 120, QStringLiteral("D120"),
        QStringLiteral("当前步骤号"), AccessType::Read, ValueType::U16, 0, 0, 5));
    m_defs.push_back(def(QStringLiteral("D"), 122, QStringLiteral("D122"),
        QStringLiteral("皮带速度"), AccessType::ReadWrite, ValueType::U16, 0, 100, 20000,
        1.0, QStringLiteral("Hz")));
    m_defs.push_back(def(QStringLiteral("D"), 126, QStringLiteral("D126"),
        QStringLiteral("调宽频率(低字)"), AccessType::Read, ValueType::U32, 127, 0, 0,
        1.0, QStringLiteral("Hz")));
    m_defs.push_back(def(QStringLiteral("D"), 127, QStringLiteral("D127"),
        QStringLiteral("调宽频率(高字)"), AccessType::Read, ValueType::U16));
    m_defs.push_back(def(QStringLiteral("D"), 128, QStringLiteral("D128"),
        QStringLiteral("目标宽度"), AccessType::ReadWrite, ValueType::U16, 0, 50, 400,
        1.0, QStringLiteral("mm")));
    m_defs.push_back(def(QStringLiteral("D"), 130, QStringLiteral("D130"),
        QStringLiteral("当前宽度"), AccessType::Read, ValueType::U16, 0, 50, 400,
        1.0, QStringLiteral("mm")));
    m_defs.push_back(def(QStringLiteral("D"), 136, QStringLiteral("D136"),
        QStringLiteral("调宽脉冲数(低字)"), AccessType::Read, ValueType::I32, 137));
    m_defs.push_back(def(QStringLiteral("D"), 137, QStringLiteral("D137"),
        QStringLiteral("调宽脉冲数(高字)"), AccessType::Read, ValueType::U16));
    m_defs.push_back(def(QStringLiteral("D"), 138, QStringLiteral("D138"),
        QStringLiteral("产量计数(低字)"), AccessType::Read, ValueType::U32, 139));
    m_defs.push_back(def(QStringLiteral("D"), 139, QStringLiteral("D139"),
        QStringLiteral("产量计数(高字)"), AccessType::Read, ValueType::U16));
    m_defs.push_back(def(QStringLiteral("D"), 140, QStringLiteral("D140"),
        QStringLiteral("心跳计数"), AccessType::Read, ValueType::U16));
    m_defs.push_back(def(QStringLiteral("D"), 204, QStringLiteral("D204"),
        QStringLiteral("脉冲当量"), AccessType::ReadWrite, ValueType::U16, 0, 1, 32767,
        1.0, QStringLiteral("脉冲/mm")));
    m_defs.push_back(def(QStringLiteral("D"), 210, QStringLiteral("D210"),
        QStringLiteral("调宽差值(目标-当前)"), AccessType::Read, ValueType::I16, 0, 0, 0,
        1.0, QStringLiteral("mm")));
    m_defs.push_back(def(QStringLiteral("D"), 220, QStringLiteral("D220"),
        QStringLiteral("调宽速度设定"), AccessType::ReadWrite, ValueType::U16, 0, 1, 15,
        1.0, QStringLiteral("mm/s")));
}

const AddressTable &AddressTable::instance()
{
    static const AddressTable table;
    return table;
}

const AddressDef *AddressTable::find(const QString &area, quint16 address) const
{
    for (const AddressDef &d : m_defs) {
        if (d.area == area && d.address == address)
            return &d;
    }
    return nullptr;
}

const AddressDef *AddressTable::findByName(const QString &name) const
{
    for (const AddressDef &d : m_defs) {
        if (d.name == name)
            return &d;
    }
    return nullptr;
}

} // namespace hlm
