#include "domain/fault_code.h"

namespace hlm {

FaultCodeTable::FaultCodeTable()
{
    // Codes 0-10 from 需求/PLC上位机地址及要求.txt §五 and spec §10.3.
    // Code 0 = no fault. Code 10 = 调宽定位超时 (latched, spec §10.3).
    m_entries.push_back({0, QStringLiteral("无故障"), false});
    m_entries.push_back({1, QStringLiteral("急停"), true});
    m_entries.push_back({2, QStringLiteral("安全门打开"), true});
    m_entries.push_back({3, QStringLiteral("安全光栅遮挡"), true});
    m_entries.push_back({4, QStringLiteral("气压低"), true});
    m_entries.push_back({5, QStringLiteral("挡停气缸伸出超时"), true});
    m_entries.push_back({6, QStringLiteral("挡停气缸缩回超时"), true});
    m_entries.push_back({7, QStringLiteral("相机测试超时"), true});
    m_entries.push_back({8, QStringLiteral("回原点反转超时"), true});
    m_entries.push_back({9, QStringLiteral("回原点正转超时"), true});
    m_entries.push_back({10, QStringLiteral("调宽定位超时"), true});
    // Code 11 = 扫码失败 (user decision 2026-09-24, from the machine): the
    // field PLC reports a failed scan cycle on D110 after the HMI's M88 扫码失败
    // answer. The supplied ladder only reaches MOV K10 D110, so this code is a
    // field-confirmed addition like code 10, not a ladder-derived one. Without
    // it the banner showed the generic 未知锁存故障 and the operator could not
    // tell a scan failure from an unknown fault.
    m_entries.push_back({11, QStringLiteral("扫码失败"), true});
}

const FaultCodeTable &FaultCodeTable::instance()
{
    static const FaultCodeTable table;
    return table;
}

FaultInfo FaultCodeTable::info(quint16 code) const
{
    for (const FaultInfo &f : m_entries) {
        if (f.code == code)
            return f;
    }
    // Unknown non-zero code: report as unknown latched fault, never crash.
    // Returned by value so concurrent queries from different threads cannot
    // race on shared mutable state.
    return {code, QStringLiteral("未知锁存故障"), true};
}

} // namespace hlm
