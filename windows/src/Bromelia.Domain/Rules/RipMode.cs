namespace Bromelia.Domain;

/// <summary>What a video-disc job makes (common.json's RipMode); audioCD and dataImage are chosen from the disc's content.</summary>
public enum RipMode
{
    Mkv,
    Backup,
    BackupDecrypted,
    BackupThenMkv,
    InfoOnly,
    AudioCD,
    DataImage,
}
