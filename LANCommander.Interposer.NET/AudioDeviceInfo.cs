namespace LANCommander.Interposer
{
    /// <summary>
    /// The direction an audio endpoint carries data in.
    /// </summary>
    public enum AudioDeviceDirection
    {
        /// <summary>The device's direction could not be determined.</summary>
        Unknown = 0,

        /// <summary>A playback (render) device: speakers, headphones, HDMI.</summary>
        Output = 1,

        /// <summary>A recording (capture) device: microphones, line-in.</summary>
        Input = 2,
    }

    /// <summary>
    /// Describes a single audio device as reported by
    /// <see cref="InterposerService.EnumerateAudioDevices"/>, including whether
    /// it passes the configured AudioDevices filter.
    /// </summary>
    public sealed class AudioDeviceInfo
    {
        /// <summary>The device's friendly name (e.g. "Speakers (Realtek High Definition Audio)").</summary>
        public string Name { get; internal set; }

        /// <summary>The device's description (e.g. "Speakers").</summary>
        public string Description { get; internal set; }

        /// <summary>The Core Audio endpoint ID, which uniquely identifies the device.</summary>
        public string Id { get; internal set; }

        /// <summary>The device instance path, e.g. <c>SWD\MMDEVAPI\{0.0.0.00000000}.{...}</c>.</summary>
        public string InstanceId { get; internal set; }

        /// <summary>
        /// The most specific hardware ID of the physical device behind the endpoint,
        /// e.g. <c>USB\VID_0965&amp;PID_9001</c>. Empty when the device tree could not be
        /// walked. This is the identifier that survives a re-plug into a different port.
        /// </summary>
        public string HardwareId { get; internal set; }

        /// <summary>Whether the device plays audio back or records it.</summary>
        public AudioDeviceDirection Direction { get; internal set; }

        /// <summary>True if this is the system default endpoint for its direction.</summary>
        public bool IsDefault { get; internal set; }

        /// <summary>
        /// True if the device passes the configured AudioDevices filter (or no
        /// filter is configured), meaning the game can see it; false if hidden.
        /// </summary>
        public bool Allowed { get; internal set; }
    }
}
