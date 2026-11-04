param(
    [ValidateSet('Explorer', 'PhoneLink')]
    [string]$As = 'Explorer'
)

$null = [Windows.UI.Notifications.ToastNotificationManager, Windows.UI.Notifications, ContentType = WindowsRuntime]
$null = [Windows.Data.Xml.Dom.XmlDocument, Windows.Data.Xml.Dom.XmlDocument, ContentType = WindowsRuntime]

$targets = @{
    Explorer  = @{
        Aumid = 'Microsoft.Windows.Explorer'
        Texts = @('Test ostrova', 'Toto je skusobne upozornenie')
    }
    PhoneLink = @{
        Aumid = 'Microsoft.YourPhone_8wekyb3d8bbwe!App'
        Texts = @('Spravy', 'Testovaci odosielatel', 'Toto je skusobna sprava pre Dynamic Island')
    }
}

$target = $targets[$As]
$escaped = $target.Texts | ForEach-Object { '<text>' + [Security.SecurityElement]::Escape($_) + '</text>' }
$xml = New-Object Windows.Data.Xml.Dom.XmlDocument
$xml.LoadXml("<toast><visual><binding template=`"ToastGeneric`">$($escaped -join '')</binding></visual></toast>")
[Windows.UI.Notifications.ToastNotificationManager]::CreateToastNotifier($target.Aumid).Show([Windows.UI.Notifications.ToastNotification]::new($xml))
