import os
import subprocess
import re

# --- CONFIGURATION ---
DPI = 1200  # You can go up to 2400 for ultra quality
EQUATIONS_FILE = "equations.txt"

# 1. Define the standalone LaTeX template with \displaystyle for everything
# This ensures ALL equations render in display style (larger, more readable)
tex_template = r"""\documentclass[border=4pt]{standalone}
\usepackage{amsmath}
\usepackage{xcolor}
\begin{document}
$\displaystyle %s $
\end{document}
"""

def read_equations_from_file(filename):
    """Read equations from a text file, one equation per line."""
    if not os.path.exists(filename):
        print(f"[-] Error: File '{filename}' not found!")
        return []
    
    with open(filename, 'r', encoding='utf-8') as f:
        equations = []
        for line_num, line in enumerate(f, 1):
            equation = line.strip()
            if equation:
                safe_name = f"equation_{line_num:03d}"
                equations.append((safe_name, equation))
                print(f"[*] Line {line_num}: {equation[:60]}{'...' if len(equation) > 60 else ''}")
        
    print(f"\n[+] Loaded {len(equations)} equations from '{filename}'\n")
    return equations

def validate_latex(equation):
    """Basic validation to check if the equation might be valid LaTeX."""
    if not equation:
        return False
    
    # Check for balanced curly braces
    if equation.count('{') != equation.count('}'):
        print(f"[-] Warning: Unbalanced curly braces in equation")
        return False
    
    return True

def render_to_high_res_png(eq_name, eq_code):
    print(f"[*] Rendering: {eq_code[:50]}{'...' if len(eq_code) > 50 else ''}")
    tex_filename = f"{eq_name}.tex"
    dvi_filename = f"{eq_name}.dvi"
    png_filename = f"{eq_name}.png"
    
    # Skip if PNG already exists
    if os.path.exists(png_filename):
        print(f"[!] {png_filename} already exists, skipping...")
        return True
    
    if not validate_latex(eq_code):
        print(f"[-] Skipping '{eq_name}' - failed validation")
        return False
    
    # Write the LaTeX code to a file
    with open(tex_filename, "w", encoding='utf-8') as f:
        f.write(tex_template % eq_code)
    
    try:
        # Step 1: Compile to DVI
        result = subprocess.run(
            ["latex", "-interaction=nonstopmode", tex_filename], 
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            shell=True
        )
        
        if result.returncode != 0 or not os.path.exists(dvi_filename):
            print(f"[-] LaTeX compilation failed for '{eq_name}'")
            cleanup_files(eq_name)
            return False
        
        # Step 2: Convert DVI to PNG with maximum quality settings
        subprocess.run(
            [
                "dvipng",
                "-D", str(DPI),           # Ultra-high resolution
                "-T", "tight",            # Crop exactly to content
                "-bg", "Transparent",     # Transparent background
                "-Q", "5",                # Maximum anti-aliasing quality (1-5, 5 is best)
                "-z", "9",                # Maximum PNG compression (1-9, 9 is best)
                "--gamma", "2.2",         # Standard gamma correction
                "--png",                  # Force PNG output
                "-o", png_filename,
                dvi_filename
            ], 
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            check=True,
            shell=True
        )
        
        # Get file size for info
        file_size = os.path.getsize(png_filename)
        size_mb = file_size / (1024 * 1024)
        
        print(f"[+] Success! {png_filename} ({size_mb:.2f} MB) at {DPI} DPI with max anti-aliasing")
        return True
        
    except subprocess.CalledProcessError as e:
        print(f"[-] Error processing '{eq_name}'")
        return False
    except Exception as e:
        print(f"[-] Unexpected error with '{eq_name}': {str(e)}")
        return False
    finally:
        cleanup_files(eq_name)

def cleanup_files(eq_name):
    """Remove temporary LaTeX build files."""
    for ext in ['.tex', '.dvi', '.log', '.aux']:
        file_to_remove = f"{eq_name}{ext}"
        if os.path.exists(file_to_remove):
            try:
                os.remove(file_to_remove)
            except:
                pass

if __name__ == "__main__":
    print("=" * 60)
    print("ULTRA QUALITY LaTeX to PNG Render Pipeline")
    print("=" * 60)
    print(f"DPI Setting: {DPI} DPI")
    print(f"Anti-aliasing: Maximum (Level 5)")
    print(f"Display Style: Enabled (\\displaystyle)")
    print(f"Input File: {EQUATIONS_FILE}")
    print("=" * 60)
    print()
    
    equations = read_equations_from_file(EQUATIONS_FILE)
    
    if not equations:
        print(f"No equations to process.")
        print(f"Create an '{EQUATIONS_FILE}' file with one equation per line.")
        print("\nExample equations.txt content:")
        print("-" * 40)
        print(r"\frac{1}{x}")
        print(r"\sqrt[3]{a^2 + b^2}")
        print(r"\int_0^\infty e^{-x^2} dx")
        print("-" * 40)
        exit(1)
    
    successful = 0
    failed = 0
    
    for name, code in equations:
        if render_to_high_res_png(name, code):
            successful += 1
        else:
            failed += 1
        print()
    
    print("=" * 60)
    print("RENDERING COMPLETE!")
    print(f"✓ Success: {successful} equations")
    print(f"✗ Failed:  {failed} equations")
    print(f"📐 All successful renders use \\displaystyle + max anti-aliasing")
    print("=" * 60)
    
    if failed > 0:
        print("\n💡 Tip: Check failed equations for LaTeX syntax errors.")
    
    if os.name == 'nt':
        input("\nPress Enter to exit...")