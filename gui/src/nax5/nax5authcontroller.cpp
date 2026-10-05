#include "nax5/nax5authcontroller.h"
#include "nax5/session/nax5sessionlifecycle.h"
#include "nax5/nax5apiclient.h"
#include "nax5/nax5authparser.h"
#include "nax5/nax5authstore.h"

#include <QGuiApplication>
#include <QTimer>
#include <QWindow>

Nax5AuthController::Nax5AuthController(QObject *parent)
    : QObject(parent)
    , api(new Nax5ApiClient(this))
    , auth_state(Nax5AuthStateUnauthenticated)
    , user_id(0)
    , email_verified(false)
    , remember_enabled(nax5AuthRememberEnabled())
    , restoring_session(false)
    , fresh_login(false)
    , pending_remember(false)
    , login_request_id(0)
    , me_request_id(0)
    , logout_request_id(0)
{
    connect(api, &Nax5ApiClient::loginFinished, this, &Nax5AuthController::onLoginFinished);
    connect(api, &Nax5ApiClient::meFinished, this, &Nax5AuthController::onMeFinished);
    connect(api, &Nax5ApiClient::logoutFinished, this, &Nax5AuthController::onLogoutFinished);
    QTimer::singleShot(0, this, &Nax5AuthController::tryRestoreSession);
}

Nax5AuthController::~Nax5AuthController()
{
    clearSessionToken();
    api->abortAll();
}

QString Nax5AuthController::savedEmail() const
{
    return nax5AuthSavedEmail();
}

QString Nax5AuthController::savedPassword() const
{
    return nax5AuthSavedPassword();
}

void Nax5AuthController::setRememberEnabled(bool enabled)
{
    if (remember_enabled == enabled)
        return;
    remember_enabled = enabled;
    nax5AuthSetRememberEnabled(enabled);
    if (!enabled)
        nax5AuthClearCredentials();
    emit rememberEnabledChanged();
    emit savedCredentialsChanged();
}

void Nax5AuthController::setState(Nax5AuthState next)
{
    if (auth_state == next)
        return;
    auth_state = next;
    emit stateChanged();
}

void Nax5AuthController::setError(Nax5AuthError error)
{
    const QString message = nax5AuthErrorMessage(error);
    if (error_message == message)
        return;
    error_message = message;
    emit errorMessageChanged();
}

void Nax5AuthController::clearAccount()
{
    user_id = 0;
    account_email.clear();
    account_city.clear();
    access_status.clear();
    email_verified = false;
    balance_seconds = -1;
    billing_enforced = false;
    console_state.clear();
    console_free_in_seconds = -1;
    console_just_freed = false;
    emit accountChanged();
}

QString Nax5AuthController::consoleText() const
{
    return nax5ConsoleStatusText(console_state, console_free_in_seconds, console_just_freed);
}

void Nax5AuthController::consoleNoticeSeen()
{
    if (!console_just_freed)
        return;
    console_just_freed = false;
    emit accountChanged();
}

void Nax5AuthController::setConsole(const QString &state, qint64 free_in_seconds, bool announce)
{
    const bool was_taken = console_state == QStringLiteral("busy") || console_state == QStringLiteral("offline");
    const bool freed = announce && was_taken && state == QStringLiteral("free");
    console_state = state;
    console_free_in_seconds = free_in_seconds;
    if (state != QStringLiteral("free"))
        console_just_freed = false;
    if (freed)
    {
        // The player was waiting, maybe in another window: blink in the taskbar.
        console_just_freed = true;
        const auto windows = QGuiApplication::topLevelWindows();
        for (QWindow *window : windows)
            if (window->isVisible())
                window->alert(0);
    }
}

QString Nax5AuthController::balanceText() const
{
    return balance_seconds < 0 ? QString() : nax5FormatPlayTime(balance_seconds);
}

void Nax5AuthController::refreshAccount()
{
    if (!authenticated() || session_token.isEmpty() || me_request_id != 0)
        return;
    // Its own id: a login or logout that starts meanwhile aborts this request without an answer,
    // and that must leave no state behind.
    refresh_request_id = api->fetchMe(session_token);
}

void Nax5AuthController::clearSessionToken()
{
    if (!session_token.isEmpty())
        session_token.fill(QLatin1Char(' '));
    session_token.clear();
}

void Nax5AuthController::tryRestoreSession()
{
    if (!remember_enabled || authenticated() || authenticating())
        return;

    const QString token = nax5AuthSavedSessionToken();
    if (!token.isEmpty())
    {
        restoring_session = true;
        setError(Nax5AuthErrorNone);
        setState(nax5AuthReduce(auth_state, Nax5AuthActionLoginClicked));
        session_token = token;
        me_request_id = api->fetchMe(session_token);
        return;
    }

    tryPasswordLoginAfterRestoreFailure();
}

void Nax5AuthController::tryPasswordLoginAfterRestoreFailure()
{
    if (!remember_enabled)
        return;
    const QString email = nax5AuthSavedEmail();
    const QString password = nax5AuthSavedPassword();
    if (email.isEmpty() || password.isEmpty())
        return;
    login(email, password, true);
}

void Nax5AuthController::login(const QString &email, const QString &password, bool remember)
{
    if (!nax5AuthCanStartLogin(auth_state))
        return;

    fresh_login = true;
    pending_remember = remember;
    if (!pending_password.isEmpty())
        pending_password.fill(QLatin1Char(' '));
    pending_password = password;
    setRememberEnabled(remember);

    setError(Nax5AuthErrorNone);
    setState(nax5AuthReduce(auth_state, Nax5AuthActionLoginClicked));
    login_request_id = api->login(email, password);
}

void Nax5AuthController::logout()
{
    login_request_id = 0;
    me_request_id = 0;
    refresh_request_id = 0;
    restoring_session = false;
    const QString token = session_token;
    clearSessionToken();
    clearAccount();
    setError(Nax5AuthErrorNone);
    setState(nax5AuthReduce(auth_state, Nax5AuthActionLogout));
    if (remember_enabled)
    {
        nax5AuthSaveCredentials(nax5AuthSavedEmail(), nax5AuthSavedPassword(), QString());
        emit savedCredentialsChanged();
    }
    if (!token.isEmpty())
        logout_request_id = api->logout(token);
    if (!pending_password.isEmpty())
        pending_password.fill(QLatin1Char(' '));
    pending_password.clear();
}

void Nax5AuthController::persistCredentialsIfNeeded()
{
    if (!pending_remember)
    {
        nax5AuthClearCredentials();
        if (remember_enabled)
            setRememberEnabled(false);
        emit savedCredentialsChanged();
        return;
    }

    nax5AuthSaveCredentials(account_email, pending_password, session_token);
    emit savedCredentialsChanged();
}

void Nax5AuthController::onLoginFinished(quint64 request_id, const Nax5LoginParseResult &result)
{
    if (request_id != login_request_id)
        return;
    login_request_id = 0;
    if (result.error != Nax5AuthErrorNone)
    {
        restoring_session = false;
        clearSessionToken();
        setError(result.error);
        setState(nax5AuthReduce(auth_state, Nax5AuthActionLoginFailed));
        return;
    }
    session_token = result.session_token;
    me_request_id = api->fetchMe(session_token);
}

void Nax5AuthController::onMeFinished(quint64 request_id, const Nax5MeParseResult &result)
{
    if (request_id != 0 && request_id == refresh_request_id)
    {
        // A background refresh: a failure must not log the player out.
        refresh_request_id = 0;
        if (result.ok && authenticated())
        {
            access_status = result.access_status;
            email_verified = result.email_verified;
            balance_seconds = result.balance_seconds;
            billing_enforced = result.billing_enforced;
            setConsole(result.console_state, result.console_free_in_seconds, true);
            emit accountChanged();
        }
        return;
    }
    if (request_id != me_request_id)
        return;
    me_request_id = 0;
    if (!result.ok)
    {
        if (restoring_session)
        {
            restoring_session = false;
            clearSessionToken();
            tryPasswordLoginAfterRestoreFailure();
            return;
        }
        clearSessionToken();
        clearAccount();
        setError(result.network_failure ? Nax5AuthErrorNetworkError : Nax5AuthErrorInvalidResponse);
        setState(nax5AuthReduce(auth_state, Nax5AuthActionLoginFailed));
        return;
    }
    restoring_session = false;
    user_id = result.user_id;
    account_email = result.email;
    account_city = result.city;
    access_status = result.access_status;
    email_verified = result.email_verified;
    balance_seconds = result.balance_seconds;
    billing_enforced = result.billing_enforced;
    setConsole(result.console_state, result.console_free_in_seconds, false);
    emit accountChanged();
    setError(Nax5AuthErrorNone);
    setState(nax5AuthReduce(auth_state, Nax5AuthActionLoginSucceeded));
    if (fresh_login)
    {
        persistCredentialsIfNeeded();
        fresh_login = false;
        if (!pending_password.isEmpty())
            pending_password.fill(QLatin1Char(' '));
        pending_password.clear();
    }
    else if (remember_enabled)
    {
        nax5AuthSaveCredentials(account_email, nax5AuthSavedPassword(), session_token);
        emit savedCredentialsChanged();
    }
}

void Nax5AuthController::onLogoutFinished(quint64 request_id)
{
    if (request_id != logout_request_id)
        return;
    logout_request_id = 0;
}
