Add-Type -AssemblyName System.Runtime.WindowsRuntime
$asTask = ([System.WindowsRuntimeSystemExtensions].GetMethods() | ? { $_.Name -eq 'AsTask' -and $_.GetParameters().Count -eq 1 -and $_.GetParameters()[0].ParameterType.Name -eq 'IAsyncOperation`1' })[0]
function Await($op, [Type]$t) { $task = $asTask.MakeGenericMethod($t).Invoke($null, @($op)); $task.Wait(-1) | Out-Null; $task.Result }
$null = [Windows.UI.Notifications.Management.UserNotificationListener, Windows.UI.Notifications, ContentType = WindowsRuntime]
$l = [Windows.UI.Notifications.Management.UserNotificationListener]::Current
$acc = Await ($l.RequestAccessAsync()) ([Windows.UI.Notifications.Management.UserNotificationListenerAccessStatus])
"Access: $acc"
$list = Await ($l.GetNotificationsAsync([Windows.UI.Notifications.NotificationKinds]::Toast)) ([System.Collections.Generic.IReadOnlyList[Windows.UI.Notifications.UserNotification]])
"Count: $($list.Count)"
foreach ($n in $list) {
  $app = try { $n.AppInfo.DisplayInfo.DisplayName } catch { "?" }
  $b = $n.Notification.Visual.GetBinding([Windows.UI.Notifications.KnownNotificationBindings]::ToastGeneric)
  $texts = if ($b) { ($b.GetTextElements() | % { $_.Text }) -join ' | ' } else { "(no ToastGeneric binding)" }
  "[$($n.Id)] $app @ $($n.CreationTime): $texts"
}
