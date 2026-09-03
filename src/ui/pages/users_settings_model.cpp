#include "ui/pages/users_settings_model.h"

#include "ui/shell/shell_model.h"

namespace hlm {

namespace {
constexpr int kSessionWarningSeconds = 60; // 提前 60 秒提示 (spec §11.5)
} // namespace

UsersSettingsModel::UsersSettingsModel(const ShellModel &model, QObject *parent)
    : QObject(parent)
    , m_model(model)
{
}

bool UsersSettingsModel::locked() const
{
    return !isAdmin();
}

bool UsersSettingsModel::isAdmin() const
{
    return m_model.role() == Role::Admin;
}

void UsersSettingsModel::setNeedsInitialAdmin(bool needs)
{
    if (m_needsInitialAdmin == needs)
        return;
    m_needsInitialAdmin = needs;
    emit stateChanged();
}

void UsersSettingsModel::setLoginResult(const LoginResult &result)
{
    m_loginLocked = !result.ok && result.reason == QStringLiteral("locked");
    m_loginError = result.ok ? QString()
                             : (result.reason == QStringLiteral("bad credentials")
                                    ? QStringLiteral("用户名或密码错误")
                                    : result.reason);
    emit stateChanged();
}

QString UsersSettingsModel::loginStatusText() const
{
    if (m_loginLocked)
        return QStringLiteral("登录失败次数过多, 账号已锁定 30 秒");
    return m_loginError;
}

void UsersSettingsModel::setSessionRemainingSec(int seconds)
{
    m_sessionRemainingSec = seconds;
    m_sessionWarning = seconds > 0 && seconds <= kSessionWarningSeconds;
    m_sessionExpired = seconds <= 0;
    emit stateChanged();
}

QString UsersSettingsModel::sessionStatusText() const
{
    if (m_sessionExpired)
        return QStringLiteral("会话已超时, 正在注销");
    if (m_sessionWarning)
        return QStringLiteral("会话即将自动注销, 剩余 %1 秒")
            .arg(m_sessionRemainingSec);
    return QStringLiteral("会话剩余 %1 秒").arg(m_sessionRemainingSec);
}

void UsersSettingsModel::setUsers(const QVector<UserRecord> &users)
{
    m_users = users;
    emit stateChanged();
}

void UsersSettingsModel::setSerialConfig(const SerialConfig &cfg)
{
    m_serial = cfg;
    emit stateChanged();
}

bool UsersSettingsModel::serialConfigValid() const
{
    return serialConfigReasons().isEmpty();
}

QStringList UsersSettingsModel::serialConfigReasons() const
{
    QStringList reasons;
    if (m_serial.comPort.trimmed().isEmpty())
        reasons.append(QStringLiteral("COM 口不能为空"));
    if (m_serial.station < 1 || m_serial.station > 247)
        reasons.append(QStringLiteral("站号需在 1-247 之间"));
    if (m_serial.baudRate != 9600 && m_serial.baudRate != 19200)
        reasons.append(QStringLiteral("波特率仅支持 9600 或 19200"));
    if (m_serial.stopBits != 1 && m_serial.stopBits != 2)
        reasons.append(QStringLiteral("停止位仅支持 1 或 2"));
    if (m_serial.parity != QStringLiteral("无")
        && m_serial.parity != QStringLiteral("奇")
        && m_serial.parity != QStringLiteral("偶"))
        reasons.append(QStringLiteral("校验仅支持 无/奇/偶"));
    if (m_serial.timeoutMs <= 0)
        reasons.append(QStringLiteral("超时需大于 0"));
    if (m_serial.readRetries < 0)
        reasons.append(QStringLiteral("读重试次数不能为负"));
    return reasons;
}

void UsersSettingsModel::setEditedD122(int v)
{
    m_editedD122 = v;
    emit stateChanged();
}

void UsersSettingsModel::setEditedD204(int v)
{
    m_editedD204 = v;
    emit stateChanged();
}

void UsersSettingsModel::setEditedD220(int v)
{
    m_editedD220 = v;
    emit stateChanged();
}

bool UsersSettingsModel::d122Valid() const
{
    return m_editedD122 >= 100 && m_editedD122 <= 20000;
}

bool UsersSettingsModel::d204Valid() const
{
    return m_editedD204 >= 1 && m_editedD204 <= 32767;
}

bool UsersSettingsModel::d220Valid() const
{
    return m_editedD220 >= 1 && m_editedD220 <= 15;
}

bool UsersSettingsModel::productValid() const
{
    const qint64 product = qint64(m_editedD204) * m_editedD220;
    return product >= 10 && product <= 200000;
}

QStringList UsersSettingsModel::paramReasons() const
{
    QStringList reasons;
    if (!d122Valid())
        reasons.append(QStringLiteral("D122 皮带速度需在 100-20000 Hz 之间"));
    if (!d204Valid())
        reasons.append(QStringLiteral("D204 脉冲当量需在 1-32767 脉冲/mm 之间"));
    if (!d220Valid())
        reasons.append(QStringLiteral("D220 调宽速度需在 1-15 mm/s 之间"));
    if (!productValid())
        reasons.append(QStringLiteral("D204×D220 需在 10-200000 之间"));
    return reasons;
}

void UsersSettingsModel::setParameterWritePending()
{
    m_paramWritePending = true;
    m_paramStatus.clear();
    emit stateChanged();
}

void UsersSettingsModel::setParameterWriteResult(bool ok, const QString &detail)
{
    m_paramWritePending = false;
    m_paramStatus = ok ? QStringLiteral("写入成功")
                       : QStringLiteral("写入失败: %1").arg(detail);
    emit stateChanged();
}

QString UsersSettingsModel::paramStatusText() const
{
    if (m_paramWritePending)
        return QStringLiteral("等待 PLC 确认写入结果");
    return m_paramStatus;
}

} // namespace hlm
