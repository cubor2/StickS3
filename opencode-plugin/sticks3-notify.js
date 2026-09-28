// ------------------------------------------------------------
// sticks3-notify.js — plugin OpenCode : sonnette « frites prêtes ».
//
// Quand une IA finit son boulot (session.idle), on prévient le
// service StickS3 qui fait ding sur le stick — comme ça tu peux
// te balader dans la maison et savoir que c'est prêt.
//
// Installation : copier ce fichier dans
//   ~/.config/opencode/plugins/sticks3-notify.js   (global)
//   ou .opencode/plugins/sticks3-notify.js         (projet)
// Puis redémarrer OpenCode.
// ------------------------------------------------------------
const NOTIFY_URL = process.env.STICKS3_NOTIFY_URL || "http://127.0.0.1:8788/notify";

async function ping(title, message, sound) {
  try {
    await fetch(NOTIFY_URL, {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ title, message, sound }),
      signal: AbortSignal.timeout(800),
    });
  } catch {
    // service absent ou planté : on n'embête jamais OpenCode
  }
}

export const Sticks3Notify = async ({ project, client, $, directory, worktree }) => {
  return {
    event: async ({ event }) => {
      // session.idle = l'agent a fini de répondre, la main revient à l'humain
      if (event.type === "session.idle") {
        await ping("C'EST PRET !", "une IA a fini son boulot", "frites");
      }
      // variante possible : aussi beeper sur les erreurs de session
      // if (event.type === "session.error") {
      //   await ping("OUPS !", "erreur de session", "error");
      // }
    },
  };
};
