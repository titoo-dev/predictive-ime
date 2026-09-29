export function Mark({ size = 32 }: { size?: number }) {
  return (
    <span
      aria-hidden
      className="grid place-items-center rounded-[26%] font-extrabold text-white select-none"
      style={{
        width: size,
        height: size,
        fontSize: size * 0.58,
        letterSpacing: "-0.05em",
        background:
          "linear-gradient(135deg, var(--m-blue-l), var(--m-blue) 60%, var(--m-indigo))",
        boxShadow:
          "0 6px 16px rgba(25,118,210,0.35), inset 0 1px 0 rgba(255,255,255,0.4)",
      }}
    >
      P
    </span>
  );
}

export function Logo({ size = 32 }: { size?: number }) {
  return (
    <span className="inline-flex items-center gap-2.5">
      <Mark size={size} />
      <span
        className="font-semibold tracking-[-0.03em]"
        style={{ fontSize: size * 0.62 }}
      >
        Predict
      </span>
    </span>
  );
}
