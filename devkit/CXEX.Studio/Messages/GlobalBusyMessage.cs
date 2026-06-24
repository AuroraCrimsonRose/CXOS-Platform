using CommunityToolkit.Mvvm.Messaging.Messages;

namespace CXEX.Studio.Messages;

// Passes a tuple containing the busy state and the text to display
public class GlobalBusyMessage : ValueChangedMessage<(bool IsBusy, string StatusText)>
{
    public GlobalBusyMessage(bool isBusy, string statusText = "") : base((isBusy, statusText)) { }
}