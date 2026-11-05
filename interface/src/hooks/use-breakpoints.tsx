import { useState, useEffect } from "react"

// Breakpoints padrão do Tailwind CSS
export const BREAKPOINTS = {
  sm: 640,
  md: 768,
  lg: 1024,
  xl: 1280,
  "2xl": 1536,
} as const

export default function useBreakpoints() {
  const [width, setWidth] = useState<number>(0);

  useEffect(() => {
    // Função para atualizar a largura atual
    const handleResize = () => setWidth(window.innerWidth);

    // Define a largura inicial assim que o componente carrega
    handleResize();

    window.addEventListener("resize", handleResize);
    return () => window.removeEventListener("resize", handleResize);
  }, []);

  return {
    isSm: width >= BREAKPOINTS.sm,      // >= 640px
    isMd: width >= BREAKPOINTS.md,      // >= 768px
    isLg: width >= BREAKPOINTS.lg,      // >= 1024px
    isXl: width >= BREAKPOINTS.xl,      // >= 1280px
    is2xl: width >= BREAKPOINTS["2xl"], // >= 1536px
    isMobile: width < BREAKPOINTS.md,                  // < 768px
    isTablet: width >= BREAKPOINTS.md && width < BREAKPOINTS.lg, // 768px a 1023px
    isDesktop: width >= BREAKPOINTS.lg,                // >= 1024px
    width,
  }
}