#!/usr/bin/env python3
"""
Advanced benchmark analysis and visualization for fiskta
Usage: python3 benchmark_analysis.py [options] <benchmark_results.json>
"""

import json
import sys
import argparse
import matplotlib.pyplot as plt
import pandas as pd
import numpy as np
from pathlib import Path
import seaborn as sns
from datetime import datetime

def load_benchmark_data(json_file):
    """Load benchmark results from JSON file"""
    with open(json_file, 'r') as f:
        data = json.load(f)
    return data

def create_performance_summary(data):
    """Create a comprehensive performance summary"""
    results = data['benchmark']['results']
    config = data['benchmark']['configuration']
    
    print("=" * 60)
    print("FISKTA PERFORMANCE ANALYSIS")
    print("=" * 60)
    print(f"Binary: {data['benchmark']['binary']}")
    print(f"Timestamp: {data['benchmark']['timestamp']}")
    print(f"Configuration: {config}")
    print()
    
    # Group results by test type
    test_groups = {}
    for test_name, result in results.items():
        if result['failures'] > 0:
            continue
            
        # Extract test category
        if 'find_simple' in test_name:
            category = 'Simple Find'
        elif 'find_regex' in test_name:
            category = 'Regex Find'
        elif 'take_throughput' in test_name:
            category = 'Throughput'
        elif 'compare_' in test_name:
            category = 'Comparison'
        else:
            category = 'Other'
            
        if category not in test_groups:
            test_groups[category] = []
        test_groups[category].append((test_name, result))
    
    # Analyze each category
    for category, tests in test_groups.items():
        print(f"{category.upper()} PERFORMANCE")
        print("-" * 40)
        
        for test_name, result in tests:
            size_mb = result['file_size_mb']
            wall_time = result['wall_time']['avg']
            throughput = result['throughput_mbps']
            memory_kb = result['peak_memory_kb']['avg']
            efficiency = result['memory_efficiency']
            
            print(f"{test_name:25} ({size_mb:3d}MB): {wall_time:6.3f}s, {throughput:6.1f} MB/s, {memory_kb:6.0f}KB, {efficiency:.3f} MB/MB")
        
        print()

def analyze_scalability(data):
    """Analyze scalability across different file sizes"""
    results = data['benchmark']['results']
    
    # Group by test type and analyze scaling
    scalability_data = {}
    
    for test_name, result in results.items():
        if result['failures'] > 0:
            continue
            
        # Extract base test name (without size suffix)
        base_name = test_name
        for suffix in ['_small', '_medium', '_large']:
            if base_name.endswith(suffix):
                base_name = base_name[:-len(suffix)]
                break
        
        if base_name not in scalability_data:
            scalability_data[base_name] = []
        
        scalability_data[base_name].append({
            'size_mb': result['file_size_mb'],
            'wall_time': result['wall_time']['avg'],
            'throughput': result['throughput_mbps'],
            'memory_kb': result['peak_memory_kb']['avg'],
            'efficiency': result['memory_efficiency']
        })
    
    print("SCALABILITY ANALYSIS")
    print("=" * 40)
    
    for test_name, data_points in scalability_data.items():
        if len(data_points) < 2:
            continue
            
        # Sort by file size
        data_points.sort(key=lambda x: x['size_mb'])
        
        print(f"\n{test_name.upper()}:")
        print("Size(MB)  Time(s)   Throughput(MB/s)  Memory(KB)  Efficiency")
        print("-" * 55)
        
        for dp in data_points:
            print(f"{dp['size_mb']:8d}  {dp['wall_time']:7.3f}   {dp['throughput']:13.1f}  {dp['memory_kb']:9.0f}  {dp['efficiency']:9.3f}")
        
        # Calculate scaling efficiency
        if len(data_points) >= 2:
            first = data_points[0]
            last = data_points[-1]
            size_ratio = last['size_mb'] / first['size_mb']
            time_ratio = last['wall_time'] / first['wall_time']
            
            if time_ratio > 0:
                scaling_efficiency = size_ratio / time_ratio
                print(f"Scaling efficiency: {scaling_efficiency:.2f} (ideal = {size_ratio:.1f})")

def create_visualizations(data, output_dir):
    """Create performance visualization charts"""
    results = data['benchmark']['results']
    
    # Prepare data for plotting
    plot_data = []
    for test_name, result in results.items():
        if result['failures'] > 0:
            continue
            
        plot_data.append({
            'test_name': test_name,
            'size_mb': result['file_size_mb'],
            'wall_time': result['wall_time']['avg'],
            'throughput': result['throughput_mbps'],
            'memory_kb': result['peak_memory_kb']['avg'],
            'efficiency': result['memory_efficiency']
        })
    
    df = pd.DataFrame(plot_data)
    
    # Set up the plotting style
    plt.style.use('seaborn-v0_8')
    sns.set_palette("husl")
    
    # Create output directory
    output_path = Path(output_dir)
    output_path.mkdir(exist_ok=True)
    
    # 1. Throughput vs File Size
    plt.figure(figsize=(12, 8))
    
    # Group by test type
    test_types = {}
    for _, row in df.iterrows():
        test_name = row['test_name']
        if 'find_simple' in test_name:
            test_type = 'Simple Find'
        elif 'find_regex' in test_name:
            test_type = 'Regex Find'
        elif 'take_throughput' in test_name:
            test_type = 'Throughput'
        elif 'compare_' in test_name:
            test_type = 'Comparison'
        else:
            test_type = 'Other'
        
        if test_type not in test_types:
            test_types[test_type] = {'sizes': [], 'throughputs': []}
        test_types[test_type]['sizes'].append(row['size_mb'])
        test_types[test_type]['throughputs'].append(row['throughput'])
    
    for test_type, data in test_types.items():
        plt.plot(data['sizes'], data['throughputs'], 'o-', label=test_type, linewidth=2, markersize=8)
    
    plt.xlabel('File Size (MB)')
    plt.ylabel('Throughput (MB/s)')
    plt.title('fiskta Performance: Throughput vs File Size')
    plt.legend()
    plt.grid(True, alpha=0.3)
    plt.xscale('log')
    plt.yscale('log')
    plt.tight_layout()
    plt.savefig(output_path / 'throughput_vs_size.png', dpi=300, bbox_inches='tight')
    plt.close()
    
    # 2. Memory Usage vs File Size
    plt.figure(figsize=(12, 8))
    
    test_types = {}
    for _, row in df.iterrows():
        test_name = row['test_name']
        if 'find_simple' in test_name:
            test_type = 'Simple Find'
        elif 'find_regex' in test_name:
            test_type = 'Regex Find'
        elif 'take_throughput' in test_name:
            test_type = 'Throughput'
        elif 'compare_' in test_name:
            test_type = 'Comparison'
        else:
            test_type = 'Other'
        
        if test_type not in test_types:
            test_types[test_type] = {'sizes': [], 'memory': []}
        test_types[test_type]['sizes'].append(row['size_mb'])
        test_types[test_type]['memory'].append(row['memory_kb'])
    
    for test_type, data in test_types.items():
        plt.plot(data['sizes'], data['memory'], 'o-', label=test_type, linewidth=2, markersize=8)
    
    plt.xlabel('File Size (MB)')
    plt.ylabel('Peak Memory Usage (KB)')
    plt.title('fiskta Memory Usage vs File Size')
    plt.legend()
    plt.grid(True, alpha=0.3)
    plt.xscale('log')
    plt.yscale('log')
    plt.tight_layout()
    plt.savefig(output_path / 'memory_vs_size.png', dpi=300, bbox_inches='tight')
    plt.close()
    
    # 3. Performance Comparison (if comparison data exists)
    comparison_data = [row for _, row in df.iterrows() if 'compare_' in row['test_name']]
    if comparison_data:
        plt.figure(figsize=(12, 8))
        
        test_names = [row['test_name'].replace('compare_', '') for row in comparison_data]
        throughputs = [row['throughput'] for row in comparison_data]
        
        bars = plt.bar(test_names, throughputs, color=['#1f77b4', '#ff7f0e', '#2ca02c'])
        plt.xlabel('Tool')
        plt.ylabel('Throughput (MB/s)')
        plt.title('Performance Comparison: fiskta vs Standard Tools')
        plt.grid(True, alpha=0.3, axis='y')
        
        # Add value labels on bars
        for bar, throughput in zip(bars, throughputs):
            plt.text(bar.get_x() + bar.get_width()/2, bar.get_height() + 1,
                    f'{throughput:.1f}', ha='center', va='bottom')
        
        plt.tight_layout()
        plt.savefig(output_path / 'performance_comparison.png', dpi=300, bbox_inches='tight')
        plt.close()
    
    # 4. Memory Efficiency Heatmap
    plt.figure(figsize=(14, 10))
    
    # Create a pivot table for memory efficiency
    pivot_data = df.pivot_table(values='efficiency', index='test_name', columns='size_mb', aggfunc='mean')
    
    sns.heatmap(pivot_data, annot=True, fmt='.3f', cmap='YlOrRd', cbar_kws={'label': 'Memory Efficiency (MB/MB)'})
    plt.title('Memory Efficiency Heatmap')
    plt.xlabel('File Size (MB)')
    plt.ylabel('Test Name')
    plt.tight_layout()
    plt.savefig(output_path / 'memory_efficiency_heatmap.png', dpi=300, bbox_inches='tight')
    plt.close()
    
    print(f"Visualizations saved to: {output_path}")

def generate_html_report(data, output_file):
    """Generate an HTML report with embedded charts"""
    results = data['benchmark']['results']
    config = data['benchmark']['configuration']
    
    html_content = f"""
<!DOCTYPE html>
<html>
<head>
    <title>fiskta Benchmark Report</title>
    <style>
        body {{ font-family: Arial, sans-serif; margin: 40px; }}
        .header {{ background-color: #f0f0f0; padding: 20px; border-radius: 5px; }}
        .section {{ margin: 20px 0; }}
        table {{ border-collapse: collapse; width: 100%; }}
        th, td {{ border: 1px solid #ddd; padding: 8px; text-align: left; }}
        th {{ background-color: #f2f2f2; }}
        .metric {{ font-weight: bold; color: #2c3e50; }}
        .good {{ color: #27ae60; }}
        .warning {{ color: #f39c12; }}
        .poor {{ color: #e74c3c; }}
    </style>
</head>
<body>
    <div class="header">
        <h1>fiskta Benchmark Report</h1>
        <p><strong>Binary:</strong> {data['benchmark']['binary']}</p>
        <p><strong>Timestamp:</strong> {data['benchmark']['timestamp']}</p>
        <p><strong>Configuration:</strong> {config}</p>
    </div>
    
    <div class="section">
        <h2>Performance Summary</h2>
        <table>
            <tr>
                <th>Test</th>
                <th>Size (MB)</th>
                <th>Wall Time (s)</th>
                <th>Throughput (MB/s)</th>
                <th>Memory (KB)</th>
                <th>Efficiency</th>
            </tr>
"""
    
    for test_name, result in results.items():
        if result['failures'] > 0:
            continue
            
        wall_time = result['wall_time']['avg']
        throughput = result['throughput_mbps']
        memory_kb = result['peak_memory_kb']['avg']
        efficiency = result['memory_efficiency']
        
        # Color code performance metrics
        throughput_class = "good" if throughput > 100 else "warning" if throughput > 50 else "poor"
        efficiency_class = "good" if efficiency < 0.5 else "warning" if efficiency < 1.0 else "poor"
        
        html_content += f"""
            <tr>
                <td>{test_name}</td>
                <td>{result['file_size_mb']}</td>
                <td>{wall_time:.3f}</td>
                <td class="{throughput_class}">{throughput:.1f}</td>
                <td>{memory_kb:.0f}</td>
                <td class="{efficiency_class}">{efficiency:.3f}</td>
            </tr>
"""
    
    html_content += """
        </table>
    </div>
    
    <div class="section">
        <h2>Legend</h2>
        <p><span class="good">Green</span>: Good performance</p>
        <p><span class="warning">Orange</span>: Acceptable performance</p>
        <p><span class="poor">Red</span>: Poor performance</p>
    </div>
</body>
</html>
"""
    
    with open(output_file, 'w') as f:
        f.write(html_content)
    
    print(f"HTML report saved to: {output_file}")

def main():
    parser = argparse.ArgumentParser(description='Analyze fiskta benchmark results')
    parser.add_argument('json_file', help='JSON benchmark results file')
    parser.add_argument('--output-dir', '-o', default='benchmark_analysis', 
                       help='Output directory for visualizations')
    parser.add_argument('--html-report', '-r', help='Generate HTML report')
    parser.add_argument('--no-visualizations', action='store_true',
                       help='Skip generating visualizations')
    
    args = parser.parse_args()
    
    # Load benchmark data
    try:
        data = load_benchmark_data(args.json_file)
    except FileNotFoundError:
        print(f"Error: File {args.json_file} not found")
        sys.exit(1)
    except json.JSONDecodeError:
        print(f"Error: Invalid JSON in {args.json_file}")
        sys.exit(1)
    
    # Generate analysis
    create_performance_summary(data)
    analyze_scalability(data)
    
    # Generate visualizations
    if not args.no_visualizations:
        try:
            create_visualizations(data, args.output_dir)
        except ImportError as e:
            print(f"Warning: Could not generate visualizations: {e}")
            print("Install matplotlib, pandas, and seaborn for visualization support")
    
    # Generate HTML report
    if args.html_report:
        generate_html_report(data, args.html_report)

if __name__ == '__main__':
    main()
