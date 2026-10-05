#pragma once

#include "nax5/nax5autherror.h"
#include "nax5/nax5authstate.h"

#include <QObject>
#include <QString>

class Nax5ApiClient;
struct Nax5LoginParseResult;
struct Nax5MeParseResult;

class Nax5AuthController : public QObject
{
    Q_OBJECT
    Q_PROPERTY(int state READ state NOTIFY stateChanged)
    Q_PROPERTY(bool authenticating READ authenticating NOTIFY stateChanged)
    Q_PROPERTY(bool authenticated READ authenticated NOTIFY stateChanged)
    Q_PROPERTY(QString errorMessage READ errorMessage NOTIFY errorMessageChanged)
    Q_PROPERTY(qint64 userId READ userId NOTIFY accountChanged)
    Q_PROPERTY(QString email READ email NOTIFY accountChanged)
    Q_PROPERTY(QString city READ city NOTIFY accountChanged)
    Q_PROPERTY(QString accessStatus READ accessStatus NOTIFY accountChanged)
    Q_PROPERTY(bool emailVerified READ emailVerified NOTIFY accountChanged)
    Q_PROPERTY(bool billingEnforced READ billingEnforced NOTIFY accountChanged)
    Q_PROPERTY(QString balanceText READ balanceText NOTIFY accountChanged)
    Q_PROPERTY(bool rememberEnabled READ rememberEnabled WRITE setRememberEnabled NOTIFY rememberEnabledChanged)
    Q_PROPERTY(QString savedEmail READ savedEmail NOTIFY savedCredentialsChanged)
    Q_PROPERTY(QString savedPassword READ savedPassword NOTIFY savedCredentialsChanged)

public:
    explicit Nax5AuthController(QObject *parent = nullptr);
    ~Nax5AuthController();

    int state() const { return static_cast<int>(auth_state); }
    bool authenticating() const { return auth_state == Nax5AuthStateAuthenticating; }
    bool authenticated() const { return auth_state == Nax5AuthStateAuthenticated; }
    QString errorMessage() const { return error_message; }
    qint64 userId() const { return user_id; }
    QString email() const { return account_email; }
    QString city() const { return account_city; }
    QString accessStatus() const { return access_status; }
    bool emailVerified() const { return email_verified; }
    bool billingEnforced() const { return billing_enforced; }
    qint64 balanceSeconds() const { return balance_seconds; }
    QString balanceText() const;
    bool rememberEnabled() const { return remember_enabled; }
    QString savedEmail() const;
    QString savedPassword() const;

    QString sessionToken() const { return session_token; }

    Q_INVOKABLE void setRememberEnabled(bool enabled);
    Q_INVOKABLE void login(const QString &email, const QString &password, bool remember = false);
    Q_INVOKABLE void tryRestoreSession();
    Q_INVOKABLE void logout();
    // Re-reads the account (play time) without touching the login state.
    void refreshAccount();

signals:
    void stateChanged();
    void errorMessageChanged();
    void accountChanged();
    void rememberEnabledChanged();
    void savedCredentialsChanged();

private:
    void setState(Nax5AuthState next);
    void setError(Nax5AuthError error);
    void clearAccount();
    void clearSessionToken();
    void onLoginFinished(quint64 request_id, const Nax5LoginParseResult &result);
    void onMeFinished(quint64 request_id, const Nax5MeParseResult &result);
    void onLogoutFinished(quint64 request_id);
    void persistCredentialsIfNeeded();
    void tryPasswordLoginAfterRestoreFailure();

    Nax5ApiClient *api;
    Nax5AuthState auth_state;
    QString error_message;
    QString session_token;
    qint64 user_id;
    QString account_email;
    QString account_city;
    QString access_status;
    bool email_verified;
    qint64 balance_seconds = -1;
    bool billing_enforced = false;
    bool refreshing_account = false;
    bool remember_enabled;
    bool restoring_session;
    bool fresh_login;
    bool pending_remember;
    QString pending_password;
    quint64 login_request_id;
    quint64 me_request_id;
    quint64 logout_request_id;
};
