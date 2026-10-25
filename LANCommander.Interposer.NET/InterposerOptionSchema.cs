using System;
using System.IO;
using System.Text;

namespace LANCommander.Interposer
{
    /// <summary>
    /// The Interposer redistributable's identity and option schema, as shipped in this
    /// package. Read from the redistributable's own <c>Manifest.yml</c>, embedded at
    /// build time, so a host that bundles the DLL describes exactly the options the
    /// redistributable does: the same keys, groups, types and defaults.
    /// </summary>
    public static class InterposerOptionSchema
    {
        private const string ManifestResource = "LANCommander.Interposer.Redistributable.Manifest.yml";

        private static readonly Lazy<(Guid Id, string Schema)> Manifest = new(Load);

        /// <summary>
        /// The redistributable's Id. Options a LANCommander server sets for the
        /// Interposer on a game are keyed by it, so a host that injects its own copy
        /// can use it to find them.
        /// </summary>
        public static Guid RedistributableId => Manifest.Value.Id;

        /// <summary>
        /// The <c>OptionSchema</c> block: a LANCommander option schema whose dotted keys
        /// (<c>NetworkRelay.Enabled</c>, <c>RichPresence.Discord.ApplicationId</c>, ...)
        /// are Config.yml paths, except for the <c>Loader</c> group, which only the
        /// redistributable's Install script reads.
        /// </summary>
        public static string Yaml => Manifest.Value.Schema;

        private static (Guid, string) Load()
        {
            using var stream = typeof(InterposerOptionSchema).Assembly.GetManifestResourceStream(ManifestResource)
                ?? throw new InvalidOperationException($"{ManifestResource} is not embedded in {typeof(InterposerOptionSchema).Assembly.GetName().Name}.");

            using var reader = new StreamReader(stream, Encoding.UTF8);

            return Parse(reader.ReadToEnd());
        }

        /// <summary>
        /// Picks the top-level <c>Id</c> and the <c>OptionSchema: |</c> block scalar out
        /// of the manifest. Those two shapes are all the manifest's own format promises,
        /// so this reads them as text rather than taking a YAML dependency for them.
        /// </summary>
        internal static (Guid Id, string Schema) Parse(string manifest)
        {
            var id = Guid.Empty;
            var schema = new StringBuilder();
            var inSchema = false;
            var indent = -1;

            foreach (var rawLine in manifest.Split('\n'))
            {
                var line = rawLine.TrimEnd('\r');

                if (inSchema)
                {
                    // The block ends at the next top-level key; blank lines inside it are content
                    if (line.Length > 0 && !char.IsWhiteSpace(line[0]))
                        break;

                    if (line.Trim().Length == 0)
                    {
                        schema.Append('\n');
                        continue;
                    }

                    if (indent < 0)
                        indent = line.Length - line.TrimStart(' ').Length;

                    schema.Append(line.Length >= indent ? line.Substring(indent) : line.TrimStart()).Append('\n');
                    continue;
                }

                if (line.StartsWith("Id:", StringComparison.Ordinal))
                    Guid.TryParse(line.Substring(3).Trim().Trim('\'', '"'), out id);
                else if (line.StartsWith("OptionSchema:", StringComparison.Ordinal))
                    inSchema = true;
            }

            if (id == Guid.Empty || schema.Length == 0)
                throw new FormatException("The Interposer redistributable manifest has no Id or no OptionSchema block.");

            return (id, schema.ToString().TrimEnd('\n') + "\n");
        }
    }
}
